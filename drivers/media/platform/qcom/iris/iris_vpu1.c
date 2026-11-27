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
	struct platform_inst_caps *caps = inst->core->iris_platform_data->inst_caps;
	struct v4l2_pix_format_mplane *fmt = &inst->fmt_src->fmt.pix_mp;
	u32 width = max(fmt->width, inst->crop.width);
	u32 height = max(fmt->height, inst->crop.height);
	u64 mbs = (u64)NUM_MBS_PER_FRAME(height, width) * inst->frame_rate;
	u64 vpp_freq = mbs * (inst->domain == ENCODER ? 675 : caps->mb_cycles_vpp);
	u64 vsp_freq = mbs * caps->mb_cycles_vsp;
	u64 fw_freq = (u64)inst->frame_rate * caps->mb_cycles_fw;
	u64 fw_vpp_freq = (u64)inst->frame_rate * caps->mb_cycles_fw_vpp;
	u32 pipes = inst->domain == DECODER ? inst->fw_caps[PIPE].value : 1;

	vpp_freq = div_u64(vpp_freq, pipes);
	if (inst->domain == DECODER)
		vsp_freq += (u64)inst->frame_rate * data_size * 16;
	else
		vsp_freq += (u64)inst->fw_caps[BITRATE].value * 2;
	/* VPU5 budgets firmware work against the VPP load before pipeline overhead. */
	vsp_freq += max(div_u64(vsp_freq, 20),
			mul_u64_u64_div_u64(vsp_freq, fw_vpp_freq, vpp_freq));
	vpp_freq += max(div_u64(vpp_freq, 20), fw_vpp_freq);
	if (pipes > 1)
		vpp_freq += div_u64(vpp_freq, 17);

	return max3(vpp_freq, vsp_freq, fw_freq);
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
	.set_hwmode = iris_vpu1_set_hwmode,
};
