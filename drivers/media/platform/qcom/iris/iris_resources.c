// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/interconnect.h>
#include <linux/math.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/soc/qcom/llcc-qcom.h>

#include "iris_core.h"
#include "iris_resources.h"

#define BW_THRESHOLD 50000

int iris_set_icc_bw(struct iris_core *core, unsigned long icc_bw)
{
	unsigned long bw_kbps = 0, bw_prev = 0;
	const struct icc_info *icc_tbl;
	int ret = 0, i;

	icc_tbl = core->iris_platform_data->icc_tbl;

	for (i = 0; i < core->icc_count; i++) {
		if (!strcmp(core->icc_tbl[i].name, "video-mem")) {
			bw_kbps = icc_bw;
			bw_prev = core->power.icc_bw;

			bw_kbps = clamp_t(typeof(bw_kbps), bw_kbps,
					  icc_tbl[i].bw_min_kbps, icc_tbl[i].bw_max_kbps);

			if (abs_diff(bw_kbps, bw_prev) < BW_THRESHOLD && bw_prev)
				return ret;

			core->icc_tbl[i].avg_bw = bw_kbps;

			core->power.icc_bw = bw_kbps;
		} else {
			core->icc_tbl[i].avg_bw = icc_tbl[i].bw_min_kbps;
		}
	}

	return icc_bulk_set_bw(core->icc_count, core->icc_tbl);
}

int iris_unset_icc_bw(struct iris_core *core)
{
	u32 i;

	core->power.icc_bw = 0;

	for (i = 0; i < core->icc_count; i++) {
		core->icc_tbl[i].avg_bw = 0;
		core->icc_tbl[i].peak_bw = 0;
	}

	return icc_bulk_set_bw(core->icc_count, core->icc_tbl);
}

int iris_opp_set_rate(struct device *dev, unsigned long freq)
{
	struct dev_pm_opp *opp __free(put_opp) =
		devfreq_recommended_opp(dev, &freq, 0);

	if (IS_ERR(opp))
		return PTR_ERR(opp);

	return dev_pm_opp_set_opp(dev, opp);
}

int iris_enable_power_domains(struct iris_core *core, struct device *pd_dev)
{
	int ret;

	ret = iris_opp_set_rate(core->dev, ULONG_MAX);
	if (ret)
		return ret;

	return pm_runtime_resume_and_get(pd_dev);
}

int iris_disable_power_domains(struct iris_core *core, struct device *pd_dev)
{
	int ret;
	int pm_ret;

	ret = iris_opp_set_rate(core->dev, 0);

	pm_ret = pm_runtime_put_sync(pd_dev);
	if (!ret)
		ret = pm_ret;

	return ret;
}

static struct clk *iris_get_clk_by_type(struct iris_core *core, enum platform_clk_type clk_type)
{
	const struct platform_clk_data *clk_tbl;
	u32 clk_cnt, i, j;

	clk_tbl = core->iris_platform_data->clk_tbl;
	clk_cnt = core->iris_platform_data->clk_tbl_size;

	for (i = 0; i < clk_cnt; i++) {
		if (clk_tbl[i].clk_type == clk_type) {
			for (j = 0; core->clock_tbl && j < core->clk_count; j++) {
				if (!strcmp(core->clock_tbl[j].id, clk_tbl[i].clk_name))
					return core->clock_tbl[j].clk;
			}
		}
	}

	return NULL;
}

int iris_prepare_enable_clock(struct iris_core *core, enum platform_clk_type clk_type)
{
	struct clk *clock;

	clock = iris_get_clk_by_type(core, clk_type);
	if (!clock)
		return -ENOENT;

	return clk_prepare_enable(clock);
}

int iris_disable_unprepare_clock(struct iris_core *core, enum platform_clk_type clk_type)
{
	struct clk *clock;

	clock = iris_get_clk_by_type(core, clk_type);
	if (!clock)
		return -EINVAL;

	clk_disable_unprepare(clock);

	return 0;
}

static void iris_llcc_release(void *data)
{
	struct iris_core *core = data;
	u32 i;

	iris_disable_llcc(core);

	for (i = 0; i < core->llcc_count; i++) {
		llcc_slice_putd(core->llcc_slices[i]);
		core->llcc_slices[i] = NULL;
	}
	core->llcc_count = 0;
}

int iris_init_llcc(struct iris_core *core)
{
	const u32 *tbl = core->iris_platform_data->llcc_slice_tbl;
	unsigned int count = core->iris_platform_data->llcc_slice_tbl_size;
	struct llcc_slice_desc *desc;
	unsigned int i;
	int ret;

	if (!tbl || !count)
		return 0;

	core->llcc_slices = devm_kcalloc(core->dev, count, sizeof(*core->llcc_slices),
					 GFP_KERNEL);
	if (!core->llcc_slices)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		u32 uid = tbl[i];

		desc = llcc_slice_getd(uid);
		if (IS_ERR_OR_NULL(desc)) {
			ret = desc ? PTR_ERR(desc) : -ENODEV;
			while (i--) {
				llcc_slice_putd(core->llcc_slices[i]);
				core->llcc_slices[i] = NULL;
			}
			if (ret == -EPROBE_DEFER)
				return -EPROBE_DEFER;

			dev_warn(core->dev, "LLCC slice %u unavailable (%d); continuing without LLCC\n",
				 uid, ret);
			return 0;
		}
		core->llcc_slices[i] = desc;
	}

	core->llcc_count = count;

	return devm_add_action_or_reset(core->dev, iris_llcc_release, core);
}

void iris_enable_llcc(struct iris_core *core)
{
	u32 i;
	int ret;

	if (!core->llcc_count || core->llcc_activated)
		return;

	for (i = 0; i < core->llcc_count; i++) {
		ret = llcc_slice_activate(core->llcc_slices[i]);
		if (ret) {
			dev_warn(core->dev, "failed to activate LLCC slice %d: %d\n",
				 llcc_get_slice_id(core->llcc_slices[i]), ret);
			while (i--)
				llcc_slice_deactivate(core->llcc_slices[i]);
			return;
		}
	}

	core->llcc_activated = true;
}

void iris_disable_llcc(struct iris_core *core)
{
	u32 i;
	int ret;

	if (!core->llcc_activated)
		return;

	for (i = core->llcc_count; i > 0; i--) {
		ret = llcc_slice_deactivate(core->llcc_slices[i - 1]);
		if (ret)
			dev_warn(core->dev, "failed to deactivate LLCC slice %d: %d\n",
				 llcc_get_slice_id(core->llcc_slices[i - 1]), ret);
	}

	core->llcc_activated = false;
}
