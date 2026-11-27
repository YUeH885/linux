// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-mem2mem.h>

#include "iris_buffer.h"
#include "iris_instance.h"
#include "iris_power.h"
#include "iris_resources.h"
#include "iris_vpu_common.h"

#define IRIS_DCVS_WINDOW_NS		(250ULL * NSEC_PER_MSEC)
#define IRIS_DCVS_IDLE_RESET_NS		(2ULL * NSEC_PER_SEC)
#define IRIS_DCVS_DECODE_ORDER_HIGH_PCT	60
#define IRIS_DCVS_DECODE_ORDER_LOW_PCT	35
#define IRIS_DCVS_HIGH_PCT		75
#define IRIS_DCVS_LOW_PCT		40
#define IRIS_DCVS_DOWN_WINDOWS		4

static u32 iris_calc_bw(struct iris_inst *inst, struct icc_vote_data *data)
{
	const struct bw_info *bw_tbl = NULL;
	struct iris_core *core = inst->core;
	u32 num_rows, i, mbs, mbps;
	u32 icc_bw = 0;

	mbs = DIV_ROUND_UP(data->height, 16) * DIV_ROUND_UP(data->width, 16);
	mbps = mbs * data->fps;
	if (mbps == 0)
		goto exit;

	bw_tbl = core->iris_platform_data->bw_tbl_dec;
	num_rows = core->iris_platform_data->bw_tbl_dec_size;

	for (i = 0; i < num_rows; i++) {
		if (i != 0 && mbps > bw_tbl[i].mbs_per_sec)
			break;

		icc_bw = bw_tbl[i].bw_ddr;
	}

exit:
	return icc_bw;
}

static int iris_set_interconnects(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	struct iris_inst *instance;
	u64 total_bw_ddr = 0;
	int ret;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (!instance->max_input_data_size)
			continue;

		total_bw_ddr += instance->power.icc_bw;
	}

	ret = iris_set_icc_bw(core, total_bw_ddr);

	mutex_unlock(&core->lock);

	return ret;
}

static int iris_vote_interconnects(struct iris_inst *inst)
{
	const struct vpu_ops *vpu_ops = inst->core->iris_platform_data->vpu_ops;
	struct icc_vote_data *vote_data = &inst->icc_data;
	struct v4l2_format *inp_f = inst->fmt_src;

	vote_data->width = inp_f->fmt.pix_mp.width;
	vote_data->height = inp_f->fmt.pix_mp.height;
	vote_data->fps = max(inst->frame_rate, inst->operating_rate);

	inst->power.icc_bw = vpu_ops->calc_bw ?
			     vpu_ops->calc_bw(inst, vote_data) :
			     iris_calc_bw(inst, vote_data);

	return iris_set_interconnects(inst);
}

static int iris_set_clocks(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	struct iris_inst *instance;
	u64 freq = 0;
	int ret;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (!instance->max_input_data_size)
			continue;

		freq += instance->power.min_freq;
	}

	core->power.clk_freq = freq;
	ret = iris_opp_set_rate(core->dev, freq);
	mutex_unlock(&core->lock);

	return ret;
}

static void iris_power_reset_dcvs(struct iris_inst *inst)
{
	inst->power.dcvs_window_start_ns = 0;
	inst->power.dcvs_busy_start_ns = 0;
	inst->power.dcvs_busy_ns = 0;
	inst->power.dcvs_boost = 0;
	inst->power.dcvs_low_windows = 0;
	inst->power.dcvs_at_max = false;
}

void iris_power_mark_busy(struct iris_inst *inst)
{
	if (inst->domain != DECODER || inst->power.dcvs_busy_start_ns)
		return;

	inst->power.dcvs_busy_start_ns = ktime_get_ns();
}

void iris_power_mark_idle(struct iris_inst *inst)
{
	u64 now;

	if (inst->domain != DECODER || !inst->power.dcvs_busy_start_ns)
		return;

	now = ktime_get_ns();
	if (now > inst->power.dcvs_busy_start_ns)
		inst->power.dcvs_busy_ns += now - inst->power.dcvs_busy_start_ns;
	inst->power.dcvs_busy_start_ns = 0;
}

static void iris_dcvs_update(struct iris_inst *inst)
{
	struct iris_inst_power *power = &inst->power;
	bool decode_order;
	u32 high_pct, low_pct, util_pct;
	u64 now, total_ns, busy_ns;

	if (inst->domain != DECODER)
		return;

	now = ktime_get_ns();
	if (!power->dcvs_window_start_ns) {
		power->dcvs_window_start_ns = now;
		return;
	}

	total_ns = now - power->dcvs_window_start_ns;
	if (total_ns > IRIS_DCVS_IDLE_RESET_NS) {
		power->dcvs_window_start_ns = now;
		power->dcvs_busy_ns = 0;
		power->dcvs_low_windows = 0;
		power->dcvs_boost = 0;
		if (power->dcvs_busy_start_ns)
			power->dcvs_busy_start_ns = now;
		return;
	}

	if (total_ns < IRIS_DCVS_WINDOW_NS)
		return;

	busy_ns = power->dcvs_busy_ns;
	if (power->dcvs_busy_start_ns) {
		if (now > power->dcvs_busy_start_ns)
			busy_ns += now - power->dcvs_busy_start_ns;
		power->dcvs_busy_start_ns = now;
	}

	util_pct = div64_u64(min(busy_ns, total_ns) * 100, total_ns);
	power->dcvs_window_start_ns = now;
	power->dcvs_busy_ns = 0;

	decode_order = inst->fw_caps[DISPLAY_DELAY_ENABLE].value;
	high_pct = decode_order ? IRIS_DCVS_DECODE_ORDER_HIGH_PCT : IRIS_DCVS_HIGH_PCT;
	low_pct = decode_order ? IRIS_DCVS_DECODE_ORDER_LOW_PCT : IRIS_DCVS_LOW_PCT;

	if (util_pct >= high_pct) {
		power->dcvs_low_windows = 0;
		if (!power->dcvs_at_max)
			power->dcvs_boost++;
	} else if (util_pct <= low_pct && power->dcvs_boost) {
		if (++power->dcvs_low_windows >= IRIS_DCVS_DOWN_WINDOWS) {
			power->dcvs_boost--;
			power->dcvs_low_windows = 0;
		}
	} else {
		power->dcvs_low_windows = 0;
	}
}

static u64 iris_boost_opp_freq(struct device *dev, u64 freq, u32 steps, bool *at_max)
{
	unsigned long opp_rate = min_t(u64, freq, ULONG_MAX);
	struct dev_pm_opp *opp;
	unsigned long probe;

	opp = dev_pm_opp_find_freq_ceil(dev, &opp_rate);
	if (IS_ERR(opp)) {
		*at_max = true;
		return freq;
	}
	dev_pm_opp_put(opp);

	for (u32 remaining = steps; remaining; remaining--) {
		unsigned long next = opp_rate + 1;

		opp = dev_pm_opp_find_freq_ceil(dev, &next);
		if (IS_ERR(opp)) {
			*at_max = true;
			return steps ? max_t(u64, freq, opp_rate) : freq;
		}
		dev_pm_opp_put(opp);
		opp_rate = next;
	}

	probe = opp_rate + 1;
	opp = dev_pm_opp_find_freq_ceil(dev, &probe);
	if (IS_ERR(opp)) {
		*at_max = true;
	} else {
		dev_pm_opp_put(opp);
		*at_max = false;
	}

	return steps ? max_t(u64, freq, opp_rate) : freq;
}

static int iris_scale_clocks(struct iris_inst *inst)
{
	const struct vpu_ops *vpu_ops = inst->core->iris_platform_data->vpu_ops;
	struct v4l2_m2m_ctx *m2m_ctx = inst->m2m_ctx;
	struct v4l2_m2m_buffer *buffer, *n;
	struct iris_buffer *buf;
	size_t data_size = 0;
	u64 freq;

	v4l2_m2m_for_each_src_buf_safe(m2m_ctx, buffer, n) {
		struct vb2_buffer *vb = &buffer->vb.vb2_buf;
		size_t vb_size = 0;

		buf = to_iris_buffer(&buffer->vb);
		if (vb->planes[0].bytesused > vb->planes[0].data_offset)
			vb_size = vb->planes[0].bytesused - vb->planes[0].data_offset;
		data_size = max3(data_size, buf->data_size, vb_size);
	}

	if (data_size)
		inst->max_input_data_size = data_size;
	if (!inst->max_input_data_size)
		return 0;

	freq = vpu_ops->calc_freq(inst, inst->max_input_data_size);
	if (inst->domain == DECODER) {
		iris_dcvs_update(inst);
		freq = iris_boost_opp_freq(inst->core->dev, freq,
					   inst->power.dcvs_boost,
					   &inst->power.dcvs_at_max);
	}
	inst->power.min_freq = freq;

	return iris_set_clocks(inst);
}

void iris_reset_inst_power(struct iris_inst *inst)
{
	struct device *dev = inst->core->dev;

	inst->max_input_data_size = 0;
	inst->power.min_freq = 0;
	inst->power.icc_bw = 0;
	iris_power_reset_dcvs(inst);

	if (pm_runtime_get_if_active(dev) > 0) {
		iris_set_clocks(inst);
		iris_set_interconnects(inst);
		pm_runtime_put_autosuspend(dev);
	}
}

int iris_scale_power(struct iris_inst *inst)
{
	struct iris_core *core = inst->core;
	int ret;

	if (pm_runtime_suspended(core->dev)) {
		ret = pm_runtime_resume_and_get(core->dev);
		if (ret < 0)
			return ret;

		pm_runtime_put_autosuspend(core->dev);
	}

	ret = iris_scale_clocks(inst);
	if (ret)
		return ret;

	return iris_vote_interconnects(inst);
}
