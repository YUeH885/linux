// SPDX-License-Identifier: GPL-2.0-only

#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/pm_domain.h>
#include <linux/reset.h>

#include "iris_core.h"
#include "iris_instance.h"
#include "iris_vpu_common.h"
#include "iris_vpu_register_defines.h"

#define VPU1_CTRL_INIT_IDLE_MSG		BIT(30)
#define VPU1_CTRL_ERROR_STATUS		GENMASK(7, 1)
#define VPU1_CTRL_STATUS_PC_READY	BIT(8)
#define VPU1_CVP_POWER_DOMAIN		2

static const enum platform_clk_type vpu1_controller_clocks[] = {
	IRIS_AXIC_CLK, IRIS_CTRL_CLK, IRIS_AHB_CLK,
};

static const enum platform_clk_type vpu1_hardware_clocks[] = {
	IRIS_AXI_CLK, IRIS_AXI1_CLK, IRIS_HW_CLK, IRIS_CVP_HW_CLK,
};

static void iris_vpu1_interrupt_init(struct iris_core *core)
{
	u32 mask;

	mask = readl(core->reg_base + VPU1_WRAPPER_INTR_MASK);
	mask &= ~(VPU1_WRAPPER_INTR_MASK_A2HWD_BMSK |
		  VPU1_WRAPPER_INTR_MASK_A2HCPU_BMSK);
	writel(mask, core->reg_base + VPU1_WRAPPER_INTR_MASK);
}

static int iris_vpu1_boot_firmware(struct iris_core *core)
{
	u32 queue_size, status;
	int ret;

	queue_size = ALIGN(sizeof(struct iris_hfi_queue_table_header) +
			   IFACEQ_QUEUE_SIZE * IFACEQ_NUMQ, SZ_4K);

	writel(core->iface_q_table_daddr, core->reg_base + VPU1_UC_REGION_ADDR);
	writel(ALIGN(SFR_SIZE + queue_size, SZ_1M), core->reg_base + VPU1_UC_REGION_SIZE);
	writel(core->iface_q_table_daddr, core->reg_base + VPU1_QTBL_ADDR);
	writel(1, core->reg_base + VPU1_QTBL_INFO);
	writel(core->sfr_daddr + core->iris_firmware_data->core_arch,
	       core->reg_base + VPU1_SFR_ADDR);

	/* Video firmware uses the CPU queue map when no CVP DSP session is present. */
	writel(core->iface_q_table_daddr, core->reg_base + VPU1_CPU_CS_VMIMSG);
	writel(core->iface_q_table_daddr, core->reg_base + VPU1_CPU_CS_VMIMSGAG0);
	writel(SZ_1M, core->reg_base + VPU1_CPU_CS_VMIMSGAG1);
	writel(0, core->reg_base + VPU1_WRAPPER_CPU_CGC_DIS);
	writel(0, core->reg_base + VPU1_WRAPPER_CPU_CLOCK_CONFIG);
	writel(BIT(0), core->reg_base + VPU1_CTRL_INIT);

	ret = readl_poll_timeout(core->reg_base + VPU1_CTRL_STATUS, status,
				 status, 50, 100000);
	if (ret)
		return ret;

	if (status & VPU1_CTRL_ERROR_STATUS) {
		dev_err(core->dev, "firmware boot failed: status %#x\n", status);
		return -EIO;
	}

	return 0;
}

static void iris_vpu1_raise_interrupt(struct iris_core *core)
{
	writel(BIT(VPU1_CPU_IC_SOFTINT_H2A_SHFT), core->reg_base + VPU1_CPU_IC_SOFTINT);
}

static void iris_vpu1_clear_interrupt(struct iris_core *core)
{
	u32 status;

	status = readl(core->reg_base + VPU1_WRAPPER_INTR_STATUS);
	core->intr_status |= status & (VPU1_WRAPPER_INTR_STATUS_A2H_BMSK |
				      VPU1_WRAPPER_INTR_STATUS_A2HWD_BMSK);
	writel(1, core->reg_base + VPU1_CPU_CS_A2HSOFTINTCLR);
	writel(status, core->reg_base + VPU1_WRAPPER_INTR_CLEAR);
}

static int iris_vpu1_watchdog(struct iris_core *core, u32 status)
{
	if (status & VPU1_WRAPPER_INTR_STATUS_A2HWD_BMSK) {
		dev_err(core->dev, "received watchdog interrupt\n");
		return -ETIME;
	}

	return 0;
}

static int iris_vpu1_prepare_pc(struct iris_core *core)
{
	u32 status, cpu_status;
	int ret;

	status = readl(core->reg_base + VPU1_CTRL_STATUS);
	if (status & VPU1_CTRL_STATUS_PC_READY)
		return 0;

	cpu_status = readl(core->reg_base + VPU1_WRAPPER_CPU_STATUS);
	if (!(cpu_status & BIT(0)) || !(status & VPU1_CTRL_INIT_IDLE_MSG))
		return -EAGAIN;

	ret = core->hfi_sys_ops->sys_pc_prep(core);
	if (ret)
		return ret;

	ret = readl_poll_timeout(core->reg_base + VPU1_CTRL_STATUS, status,
				 status & VPU1_CTRL_STATUS_PC_READY, 250, 2500);
	if (ret)
		return -EAGAIN;

	ret = readl_poll_timeout(core->reg_base + VPU1_WRAPPER_CPU_STATUS,
				 cpu_status, cpu_status & BIT(0), 250, 2500);
	return ret ? -EAGAIN : 0;
}

static void iris_vpu1_set_preset_registers(struct iris_core *core)
{
	/* Iris 1.xx has no VPU2 wrapper preset register. */
}

static u64 iris_vpu1_calc_freq(struct iris_inst *inst, size_t data_size)
{
	struct platform_inst_caps *caps = iris_get_inst_caps(inst);
	struct v4l2_pix_format_mplane *fmt = &inst->fmt_src->fmt.pix_mp;
	u32 width = max(fmt->width, inst->crop.width);
	u32 height = max(fmt->height, inst->crop.height);
	u64 mbs = (u64)NUM_MBS_PER_FRAME(height, width) * inst->frame_rate;
	u64 vpp_freq = mbs * (inst->domain == ENCODER ? 675 : caps->mb_cycles_vpp);
	u64 vsp_freq = mbs * caps->mb_cycles_vsp;
	u64 fw_freq = (u64)inst->frame_rate * caps->mb_cycles_fw;
	u64 fw_vpp_freq = (u64)inst->frame_rate * caps->mb_cycles_fw_vpp;
	u32 pipes = inst->fw_caps[PIPE].value;

	vpp_freq = div_u64(vpp_freq, pipes);
	if (inst->domain == DECODER)
		vsp_freq += (u64)inst->frame_rate * data_size * 16;
	else
		vsp_freq += (u64)inst->fw_caps[BITRATE].value * 2;
	/* VPU5 budgets firmware work against the VPP load before pipeline overhead. */
	vsp_freq += max(div_u64(vsp_freq, 20),
			mul_u64_u64_div_u64(vsp_freq, fw_vpp_freq, vpp_freq));
	vpp_freq += max(div_u64(vpp_freq, 20), fw_vpp_freq);
	if (pipes > 1) {
		if (inst->domain == DECODER)
			vpp_freq += div_u64(vpp_freq, 17);
		else
			vpp_freq += div_u64(vpp_freq * 14, 1000);
	}

	return max3(vpp_freq, vsp_freq, fw_freq);
}

#define VPU1_BASELINE_WIDTH		1280
#define VPU1_BASELINE_HEIGHT		720

struct iris_vpu1_bw_lut {
	u32 frame_size;
	u32 frame_rate;
	u32 bitrate_mbps;
	u32 cr8_x100;
	u32 cr10_x100;
};

static const struct iris_vpu1_bw_lut iris_vpu1_bw_lut_tbl[] = {
	{ 1280 * 720,  30,  14, 128, 123 },
	{ 1280 * 720,  60,  22, 128, 123 },
	{ 1920 * 1088, 30,  40, 128, 123 },
	{ 1920 * 1088, 60,  64, 128, 123 },
	{ 2560 * 1440, 30,  64, 126, 122 },
	{ 2560 * 1440, 60, 102, 126, 122 },
	{ 3840 * 2160, 30,  84, 126, 122 },
	{ 3840 * 2160, 60, 134, 126, 122 },
	{ 4096 * 2160, 30,  88, 126, 122 },
	{ 4096 * 2160, 60, 141, 126, 122 },
	{ 4096 * 2304, 30,  96, 126, 122 },
	{ 4096 * 2304, 60, 154, 126, 122 },
};

static const struct iris_vpu1_bw_lut *iris_vpu1_lookup_bw_lut(u32 width, u32 height, u32 fps)
{
	u32 frame_size = width * height;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(iris_vpu1_bw_lut_tbl); i++) {
		if (iris_vpu1_bw_lut_tbl[i].frame_size >= frame_size &&
		    iris_vpu1_bw_lut_tbl[i].frame_rate >= fps)
			return &iris_vpu1_bw_lut_tbl[i];
	}

	return &iris_vpu1_bw_lut_tbl[ARRAY_SIZE(iris_vpu1_bw_lut_tbl) - 1];
}

static bool iris_vpu1_fmt_is_ubwc(u32 pixelformat)
{
	return pixelformat == V4L2_PIX_FMT_QC08C || pixelformat == V4L2_PIX_FMT_QC10C;
}

static u64 iris_vpu1_calc_bw_dec(struct iris_inst *inst, u32 fps,
				  const struct iris_vpu1_bw_lut *lut)
{
	u32 in_w = max_t(u32, inst->fmt_src->fmt.pix_mp.width, 1);
	u32 in_h = max_t(u32, inst->fmt_src->fmt.pix_mp.height, 1);
	u32 width = max_t(u32, in_w, VPU1_BASELINE_WIDTH);
	u32 height = max_t(u32, in_h, VPU1_BASELINE_HEIGHT);
	u32 op_width = max_t(u32, inst->fmt_dst->fmt.pix_mp.width, 1);
	u32 op_height = max_t(u32, inst->fmt_dst->fmt.pix_mp.height, 1);
	u32 pixfmt = inst->fmt_dst->fmt.pix_mp.pixelformat;
	bool is_h264 = (inst->codec == V4L2_PIX_FMT_H264);
	u32 lcu_size = is_h264 ? 16 : 32;
	u32 lcu_per_frame = DIV_ROUND_UP(width, lcu_size) * DIV_ROUND_UP(height, lcu_size);
	bool is_10bit = iris_fmt_is_10bit(pixfmt);
	bool unified_dpb_opb = !iris_split_mode_enabled(inst);
	bool opb_ubwc = iris_vpu1_fmt_is_ubwc(pixfmt);
	u32 dpb_cr_x100 = is_10bit ? lut->cr10_x100 : lut->cr8_x100;
	u64 bitrate_kbps, y_8bpp, y_10bpp, y_dpb, y_opb;
	u64 dpb_read, dpb_write, opb_write = 0, line_buf = 0;
	u64 colloc_rw, vsp_rw;

	bitrate_kbps = inst->max_input_data_size ?
		       DIV_ROUND_UP_ULL((u64)inst->max_input_data_size * fps * 8, 1000) :
		       (u64)lut->bitrate_mbps * 1000;
	bitrate_kbps = max_t(u64, bitrate_kbps, 1000);

	/* Bins-to-bit (4x write, 6x read) matches vendor msm_vidc_dyn_gov heuristics. */
	vsp_rw = div_u64(bitrate_kbps * 10, 8);
	colloc_rw = div_u64((u64)lcu_per_frame * (is_h264 ? 16 : 64) * 2 * fps, 1000);

	y_8bpp = div_u64((u64)width * height * fps, 1000);
	y_10bpp = div_u64(y_8bpp * 256, 192);
	y_dpb = is_10bit ? y_10bpp : y_8bpp;

	dpb_read = div_u64(y_dpb * 150 * 150, dpb_cr_x100 * 100);
	dpb_write = div_u64(y_dpb * 150 * 105, dpb_cr_x100 * 100);
	if (inst->core->llcc_activated)
		dpb_read = div_u64(dpb_read * 100, is_h264 ? 115 : 130);

	if (!unified_dpb_opb) {
		u32 opb_cr_x100 = opb_ubwc ? dpb_cr_x100 : 100;
		u64 scale_x100 = max_t(u64, 100,
				       div_u64((u64)in_w * in_h * 100,
					       (u64)op_width * op_height));

		y_opb = !is_10bit ? y_8bpp : (opb_ubwc ? y_10bpp : y_8bpp * 2);
		opb_write = div_u64(y_opb * 150 * 100, scale_x100 * opb_cr_x100);
	}

	if (!(inst->core->llcc_activated && is_h264)) {
		u32 tnbr = is_h264 ? 128 : 64;

		line_buf = div_u64((u64)tnbr * lcu_per_frame * 2 * fps, 1000);
	}

	return vsp_rw + colloc_rw + dpb_read + dpb_write + opb_write + line_buf;
}

static u64 iris_vpu1_calc_bw_enc(struct iris_inst *inst, u32 fps,
				  const struct iris_vpu1_bw_lut *lut)
{
	u32 out_w = max_t(u32, inst->fmt_dst->fmt.pix_mp.width, 1);
	u32 out_h = max_t(u32, inst->fmt_dst->fmt.pix_mp.height, 1);
	u32 width = max_t(u32, out_w, VPU1_BASELINE_WIDTH);
	u32 height = max_t(u32, out_h, VPU1_BASELINE_HEIGHT);
	u32 in_w = max_t(u32, inst->fmt_src->fmt.pix_mp.width, 1);
	u32 in_h = max_t(u32, inst->fmt_src->fmt.pix_mp.height, 1);
	u32 pixfmt = inst->fmt_src->fmt.pix_mp.pixelformat;
	u32 lcu_size = (inst->codec == V4L2_PIX_FMT_HEVC) ? 32 : 16;
	u32 lcu_per_frame = DIV_ROUND_UP(width, lcu_size) * DIV_ROUND_UP(height, lcu_size);
	bool is_10bit = iris_fmt_is_10bit(pixfmt);
	bool orig_ubwc = iris_vpu1_fmt_is_ubwc(pixfmt);
	u32 dpb_cr_x100 = is_10bit ? lut->cr10_x100 : lut->cr8_x100;
	u32 orig_cr_x100 = orig_ubwc ? dpb_cr_x100 : 100;
	u32 rot = inst->fw_caps[ROTATION].value;
	bool b_frames = inst->fw_caps[B_FRAME].value > 0;
	u64 bitrate_kbps, y_8bpp, y_10bpp, y_dpb, y_orig, downscale_x100;
	u64 ref_read_y, ref_read_crcb, ref_write, ref_write_overlap, orig_read;
	u64 line_buf = 0, mese_read_factor, mese_read, mese_write;
	u64 colloc_rw, vsp_rw;

	bitrate_kbps = inst->fw_caps[BITRATE].value > 0 ?
		       DIV_ROUND_UP_ULL((u64)inst->fw_caps[BITRATE].value, 1000) :
		       (u64)lut->bitrate_mbps * 1000;
	if (fps > inst->frame_rate && inst->frame_rate)
		bitrate_kbps = div_u64(bitrate_kbps * fps, inst->frame_rate);
	bitrate_kbps = max_t(u64, bitrate_kbps, 1000);

	vsp_rw = div_u64(bitrate_kbps * 4, 8) * 2 + div_u64(bitrate_kbps, 8);
	colloc_rw = div_u64((u64)lcu_per_frame * (lcu_size == 16 ? 16 : 64) * 2 * fps, 1000);

	y_8bpp = div_u64((u64)width * height * fps, 1000);
	y_10bpp = div_u64(y_8bpp * 256, 192);
	y_dpb = is_10bit ? y_10bpp : y_8bpp;

	ref_read_y = div_u64(y_dpb * 130, dpb_cr_x100);
	ref_read_crcb = div_u64(y_dpb * 50, dpb_cr_x100);
	if (b_frames) {
		ref_read_y *= 2;
		ref_read_crcb *= 2;
	}
	if (inst->core->llcc_activated)
		ref_read_crcb = div_u64(ref_read_crcb * 100, 150);

	ref_write = div_u64(y_dpb * 150, dpb_cr_x100);
	ref_write_overlap = div_u64(ref_write * 8, 108);

	y_orig = !is_10bit ? y_8bpp : (orig_ubwc ? y_10bpp : y_8bpp * 2);
	downscale_x100 = max_t(u64, 100,
			       div_u64((u64)in_w * in_h * 100, (u64)out_w * out_h));
	orig_read = div_u64(y_orig * 150 * downscale_x100, (u64)orig_cr_x100 * 100);
	if (rot == 90 || rot == 270)
		orig_read *= (lcu_size == 32) ? (is_10bit ? 3 : 1) : 2;

	if (!inst->core->llcc_activated)
		line_buf = div_u64((u64)16 * lcu_per_frame * 2 * fps, 1000);

	mese_read_factor = div_u64((u64)width * height * fps * 253, 4000ULL * orig_cr_x100);
	mese_read = div_u64(y_dpb * 137, orig_cr_x100) + mese_read_factor;
	mese_write = div_u64(((u64)width * height / 512 +
			      div_u64((u64)width * height * 100, 4 * orig_cr_x100) +
			      (u64)width * height / 128) * fps, 1000);

	return vsp_rw + colloc_rw + ref_read_y + ref_read_crcb +
	       ref_write + ref_write_overlap + orig_read + line_buf +
	       mese_read + mese_write;
}

static u32 iris_vpu1_calc_bw(struct iris_inst *inst, struct icc_vote_data *data)
{
	const struct iris_vpu1_bw_lut *lut;
	u32 width, height;
	u32 fps = data->fps;
	u64 bw_kbps;

	if (!fps)
		return 0;

	if (inst->domain == DECODER) {
		width = max_t(u32, data->width, VPU1_BASELINE_WIDTH);
		height = max_t(u32, data->height, VPU1_BASELINE_HEIGHT);
	} else {
		width = max_t(u32, inst->fmt_dst->fmt.pix_mp.width, VPU1_BASELINE_WIDTH);
		height = max_t(u32, inst->fmt_dst->fmt.pix_mp.height, VPU1_BASELINE_HEIGHT);
	}

	lut = iris_vpu1_lookup_bw_lut(width, height, fps);
	bw_kbps = inst->domain == DECODER ?
		  iris_vpu1_calc_bw_dec(inst, fps, lut) :
		  iris_vpu1_calc_bw_enc(inst, fps, lut);

	/* Account for QSMMU translation overhead. */
	bw_kbps = div_u64(bw_kbps * 103, 100);

	return min_t(u64, bw_kbps, U32_MAX);
}

static int iris_vpu1_enable_clocks(struct iris_core *core,
				   const enum platform_clk_type *clocks, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = iris_prepare_enable_clock(core, clocks[i]);
		if (ret)
			goto disable_clocks;
	}

	return 0;

disable_clocks:
	while (i--)
		iris_disable_unprepare_clock(core, clocks[i]);
	return ret;
}

static void iris_vpu1_disable_clocks(struct iris_core *core,
				     const enum platform_clk_type *clocks, unsigned int count)
{
	while (count--)
		iris_disable_unprepare_clock(core, clocks[count]);
}

static int iris_vpu1_power_on_controller(struct iris_core *core)
{
	struct device *pd = core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN];
	struct device *vcodec = core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN];
	struct device *cvp = core->pmdomain_tbl->pd_devs[VPU1_CVP_POWER_DOMAIN];
	u32 count = core->iris_platform_data->clk_rst_tbl_size;
	int ret;

	ret = iris_enable_power_domains(core, pd);
	if (ret)
		return ret;

	ret = iris_enable_power_domains(core, vcodec);
	if (ret)
		goto disable_controller;

	ret = iris_enable_power_domains(core, cvp);
	if (ret)
		goto disable_vcodec;

	ret = reset_control_bulk_assert(count, core->resets);
	if (ret)
		goto disable_cvp;

	/* Reset the AHB-to-AXI bridge before enabling the firmware CPU clocks. */
	usleep_range(150, 250);
	ret = reset_control_bulk_deassert(count, core->resets);
	if (ret)
		goto disable_cvp;

	ret = iris_vpu1_enable_clocks(core, vpu1_controller_clocks,
				      ARRAY_SIZE(vpu1_controller_clocks));
	if (!ret)
		return 0;

disable_cvp:
	iris_disable_power_domains(core, cvp);
disable_vcodec:
	iris_disable_power_domains(core, vcodec);
disable_controller:
	iris_disable_power_domains(core, pd);
	return ret;
}

static int iris_vpu1_power_off_controller(struct iris_core *core)
{
	iris_vpu1_disable_clocks(core, vpu1_controller_clocks,
				 ARRAY_SIZE(vpu1_controller_clocks));
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[VPU1_CVP_POWER_DOMAIN]);
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);
	return iris_disable_power_domains(core,
		core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);
}

static int iris_vpu1_power_on_hw(struct iris_core *core)
{
	return iris_vpu1_enable_clocks(core, vpu1_hardware_clocks,
				       ARRAY_SIZE(vpu1_hardware_clocks));
}

static void iris_vpu1_power_off_hw(struct iris_core *core)
{
	struct device *vcodec = core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN];
	struct device *cvp = core->pmdomain_tbl->pd_devs[VPU1_CVP_POWER_DOMAIN];

	dev_pm_genpd_set_hwmode(cvp, false);
	dev_pm_genpd_set_hwmode(vcodec, false);
	iris_vpu1_disable_clocks(core, vpu1_hardware_clocks,
				 ARRAY_SIZE(vpu1_hardware_clocks));
}

static int iris_vpu1_set_hwmode(struct iris_core *core)
{
	struct device *vcodec = core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN];
	struct device *cvp = core->pmdomain_tbl->pd_devs[VPU1_CVP_POWER_DOMAIN];
	int ret;

	ret = dev_pm_genpd_set_hwmode(vcodec, true);
	if (ret)
		return ret;

	ret = dev_pm_genpd_set_hwmode(cvp, true);
	if (ret)
		dev_pm_genpd_set_hwmode(vcodec, false);
	return ret;
}

const struct vpu_ops iris_vpu1_ops = {
	.boot_firmware = iris_vpu1_boot_firmware,
	.raise_interrupt = iris_vpu1_raise_interrupt,
	.clear_interrupt = iris_vpu1_clear_interrupt,
	.interrupt_init = iris_vpu1_interrupt_init,
	.watchdog = iris_vpu1_watchdog,
	.prepare_pc = iris_vpu1_prepare_pc,
	.set_preset_registers = iris_vpu1_set_preset_registers,
	.power_off_hw = iris_vpu1_power_off_hw,
	.power_on_hw = iris_vpu1_power_on_hw,
	.power_off_controller = iris_vpu1_power_off_controller,
	.power_on_controller = iris_vpu1_power_on_controller,
	.calc_freq = iris_vpu1_calc_freq,
	.calc_bw = iris_vpu1_calc_bw,
	.set_hwmode = iris_vpu1_set_hwmode,
};
