// SPDX-License-Identifier: GPL-2.0
/*
 * camss-vfe-170.c
 *
 * Qualcomm MSM Camera Subsystem - VFE (Video Front End) Module v170
 *
 * Copyright (C) 2020-2021 Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/minmax.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <uapi/linux/qcom_camss.h>

#include "camss.h"
#include "camss-csid.h"
#include "camss-vfe.h"
#include "camss-vfe-17x-stats.h"
#include "camss-vfe-17x-tuning.h"

#define VFE_GLOBAL_RESET_CMD			(0x018)
#define		GLOBAL_RESET_CMD_CORE		BIT(0)
#define		GLOBAL_RESET_CMD_CAMIF		BIT(1)
#define		GLOBAL_RESET_CMD_BUS		BIT(2)
#define		GLOBAL_RESET_CMD_BUS_BDG	BIT(3)
#define		GLOBAL_RESET_CMD_REGISTER	BIT(4)
#define		GLOBAL_RESET_CMD_PM		BIT(5)
#define		GLOBAL_RESET_CMD_BUS_MISR	BIT(6)
#define		GLOBAL_RESET_CMD_TESTGEN	BIT(7)
#define		GLOBAL_RESET_CMD_DSP		BIT(8)
#define		GLOBAL_RESET_CMD_IDLE_CGC	BIT(9)
#define		GLOBAL_RESET_CMD_RDI0		BIT(10)
#define		GLOBAL_RESET_CMD_RDI1		BIT(11)
#define		GLOBAL_RESET_CMD_RDI2		BIT(12)
#define		GLOBAL_RESET_CMD_RDI3		BIT(13)
#define		GLOBAL_RESET_CMD_VFE_DOMAIN	BIT(30)
#define		GLOBAL_RESET_CMD_RESET_BYPASS	BIT(31)

#define VFE_CORE_CFG				(0x050)
#define		CFG_PIXEL_PATTERN_YCBYCR	(0x4)
#define		CFG_PIXEL_PATTERN_YCRYCB	(0x5)
#define		CFG_PIXEL_PATTERN_CBYCRY	(0x6)
#define		CFG_PIXEL_PATTERN_CRYCBY	(0x7)
#define		CFG_COMPOSITE_REG_UPDATE_EN	BIT(4)

#define VFE_IRQ_CMD				(0x058)
#define		CMD_GLOBAL_CLEAR		BIT(0)

#define VFE_IRQ_MASK_0					(0x05c)
#define		MASK_0_CAMIF_SOF			BIT(0)
#define		MASK_0_CAMIF_EOF			BIT(1)
#define		MASK_0_RDI_REG_UPDATE(n)		BIT((n) + 5)
#define		MASK_0_IMAGE_MASTER_n_PING_PONG(n)	BIT((n) + 8)
#define		MASK_0_IMAGE_COMPOSITE_DONE_n(n)	BIT((n) + 25)
#define		MASK_0_RESET_ACK			BIT(31)

#define VFE_IRQ_MASK_1					(0x060)
#define		MASK_1_CAMIF_ERROR			BIT(0)
#define		MASK_1_VIOLATION			BIT(7)
#define		MASK_1_BUS_BDG_HALT_ACK			BIT(8)
#define		MASK_1_IMAGE_MASTER_n_BUS_OVERFLOW(n)	BIT((n) + 9)
#define		MASK_1_RDI_SOF(n)			BIT((n) + 29)

#define VFE_IRQ_CLEAR_0					(0x064)
#define VFE_IRQ_CLEAR_1					(0x068)

#define VFE_IRQ_STATUS_0				(0x06c)
#define		STATUS_0_CAMIF_SOF			BIT(0)
#define		STATUS_0_RDI_REG_UPDATE(n)		BIT((n) + 5)
#define		STATUS_0_BUS_WR_IRQ			BIT(9)
#define		STATUS_0_IMAGE_MASTER_PING_PONG(n)	BIT((n) + 8)
#define		STATUS_0_IMAGE_COMPOSITE_DONE(n)	BIT((n) + 25)
#define		STATUS_0_RESET_ACK			BIT(31)

#define VFE_IRQ_STATUS_1				(0x070)
#define		STATUS_1_VIOLATION			BIT(7)
#define		STATUS_1_BUS_BDG_HALT_ACK		BIT(8)
#define		STATUS_1_RDI_SOF(n)			BIT((n) + 27)

#define VFE_VIOLATION_STATUS			(0x07c)

#define VFE_MODULE_LENS_CGC_OVERRIDE		(0x02c)
#define VFE_MODULE_LENS_EN			(0x040)
#define		LENS_DEMUX_EN			BIT(2)
#define		LENS_BPC_EN			BIT(6)
#define		LENS_ABF_EN			BIT(7)
#define		LENS_ROLLOFF_EN			BIT(8)
#define		LENS_DEMOSAIC_EN		BIT(10)
#define		LENS_BLS_EN			BIT(11)

#define VFE_MODULE_COLOR_EN			(0x048)
#define		COLOR_CC_EN			BIT(1)
#define		COLOR_GTM_EN			BIT(2)
#define		COLOR_GAMMA_EN			BIT(3)
#define VFE_MODULE_COLOR_CGC_OVERRIDE		(0x034)

#define VFE_MODULE_ZOOM_EN			(0x04c)
#define		ZOOM_CST_EN			BIT(0)
#define		ZOOM_SCALE_VID_EN		BIT(3)
#define		ZOOM_CROP_VID_EN		BIT(4)

#define VFE_CAMIF_CMD				(0x478)
#define		CMD_CLEAR_CAMIF_STATUS		BIT(2)

#define VFE_CAMIF_CFG				(0x47c)
#define		CFG_VSYNC_SYNC_EDGE		(0)
#define			VSYNC_ACTIVE_HIGH	(0)
#define			VSYNC_ACTIVE_LOW	(1)
#define		CFG_HSYNC_SYNC_EDGE		(1)
#define			HSYNC_ACTIVE_HIGH	(0)
#define			HSYNC_ACTIVE_LOW	(1)
#define		CFG_VFE_SUBSAMPLE_ENABLE	BIT(4)
#define		CFG_BUS_SUBSAMPLE_ENABLE	BIT(5)
#define		CFG_VFE_OUTPUT_EN		BIT(6)
#define		CFG_BUS_OUTPUT_EN		BIT(7)
#define		CFG_BINNING_EN			BIT(9)
#define		CFG_FRAME_BASED_EN		BIT(10)
#define		CFG_RAW_CROP_EN			BIT(22)

#define VFE_CAMIF_RAW_CROP_WIDTH_CFG		(0x0ce4)
#define VFE_CAMIF_RAW_CROP_HEIGHT_CFG		(0x0ce8)
#define VFE_CAMIF_LINE_SKIP_PATTERN		(0x488)
#define VFE_CAMIF_PIXEL_SKIP_PATTERN		(0x48c)
#define VFE_CAMIF_SKIP_PERIOD			(0x490)
#define VFE_CAMIF_IRQ_SUBSAMPLE_PATTERN		(0x49c)

#define VFE_DEMUX_CFG				(0x560)

#define VFE_BPC_CFG_0				(0x5c4)
#define VFE_ABF_CFG_0				(0x5e8)
#define VFE_ABF_CFG_1				(0x5f4)
#define VFE_ROLLOFF_CFG_0			(0x6bc)

#define VFE_DEMO_CFG				(0x6f8)
#define VFE_DEMO_WB_CFG_0			(0x6fc)
#define VFE_DEMO_WB_CFG_1			(0x700)
#define VFE_DEMO_WB_OFFSET_CFG_0		(0x704)
#define VFE_DEMO_WB_OFFSET_CFG_1		(0x708)
#define VFE_DEMO_INTERP_COEFF_CFG		(0x71c)
#define VFE_DEMO_INTERP_CLASSIFIER_0		(0x720)
#define VFE_BLS_CFG_0				(0x6b0)
#define VFE_BLS_CFG_1				(0x6b4)
#define VFE_BLS_CFG_2				(0x6b8)

#define VFE_COLOR_CORRECT_COEFF_0		(0x760)
#define VFE_COLOR_CORRECT_OFFSET_0		(0x784)
#define VFE_COLOR_CORRECT_COEFF_Q		(0x790)

#define VFE_GTM_CFG				(0x794)
#define VFE_RGB_LUT_CFG			(0x798)

#define VFE_DMI_CFG				(0x0c24)
#define VFE_DMI_ADDR				(0x0c28)
#define VFE_DMI_DATA_HI			(0x0c2c)
#define VFE_DMI_DATA_LO			(0x0c30)
#define		DMI_CFG_AUTO_INCREMENT		(0x100)
#define		DMI_ABF_BANK_0			(0x0c)
#define		DMI_LSC_BANK_0_T0		(0x0e)
#define		DMI_LSC_BANK_0_T1		(0x0f)
#define		DMI_LSC_BANK_1_T0		(0x12)
#define		DMI_LSC_BANK_1_T1		(0x13)
#define		DMI_GTM_BANK_0			(0x18)
#define		DMI_GAMMA_G_BANK_0		(0x1a)
#define		DMI_GAMMA_B_BANK_0		(0x1c)
#define		DMI_GAMMA_R_BANK_0		(0x1e)

#define VFE_FULL_SCALE_Y_CFG			(0xa3c)
#define VFE_FULL_SCALE_C_CFG			(0xa68)
#define VFE_SCALE_H_SIZE(base)			((base) + 4)
#define VFE_SCALE_H_PHASE(base)			((base) + 8)
#define VFE_SCALE_H_STRIPE(base)			((base) + 12)
#define VFE_SCALE_H_PHASE_INIT(base)		((base) + 16)
#define VFE_SCALE_H_PAD(base)			((base) + 20)
#define VFE_SCALE_V_SIZE(base)			((base) + 24)
#define VFE_SCALE_V_PHASE(base)			((base) + 28)
#define VFE_SCALE_V_STRIPE(base)			((base) + 32)
#define VFE_SCALE_V_PHASE_INIT(base)		((base) + 36)
#define VFE_SCALE_V_PAD(base)			((base) + 40)
#define		SCALE_H_EN			BIT(0)
#define		SCALE_V_EN			BIT(1)

#define VFE_FULL_OUT_Y_CROP_RND_CLAMP_CFG	(0x0e0c)
#define VFE_FULL_OUT_Y_CROP_PIXEL_CFG		(0x0e14)
#define VFE_FULL_OUT_Y_CROP_LINE_CFG		(0x0e10)
#define VFE_FULL_OUT_Y_CH0_CLAMP_CFG		(0x0e18)
#define VFE_FULL_OUT_C_CROP_RND_CLAMP_CFG	(0x0e2c)
#define VFE_FULL_OUT_C_CROP_PIXEL_CFG		(0x0e34)
#define VFE_FULL_OUT_C_CROP_LINE_CFG		(0x0e30)
#define VFE_FULL_OUT_C_CH0_CLAMP_CFG		(0x0e38)
#define VFE_FULL_OUT_Y_ROUND_CFG			(0xe1c)
#define VFE_FULL_OUT_C_ROUND_CFG			(0xe3c)
#define		FULL_OUT_CROP_EN			BIT(9)
#define		FULL_OUT_ROUND_EN		BIT(10)
#define		FULL_OUT_CLAMP_EN		BIT(11)
#define		FULL_OUT_CLAMP_MIN(n)		((n) & 0x3ff)
#define		FULL_OUT_CLAMP_MAX(n)		((n) << 20)
#define		FULL_OUT_ROUND_SHIFT(n)		((n) << 3)

#define VFE_COLOR_XFORM_CH0_COEFF_CFG_0		(0x0f30)

#define VFE_REG_UPDATE_CMD			(0x4ac)
#define		REG_UPDATE_RDI(n)		BIT(1 + (n))
#define		REG_UPDATE_PIX			BIT(0)
#define		STATUS_0_PIX_REG_UPDATE		BIT(4)

#define VFE_BUS_IRQ_MASK(n)		(0x2044 + (n) * 4)
#define VFE_BUS_IRQ_CLEAR(n)		(0x2050 + (n) * 4)
#define VFE_BUS_IRQ_STATUS(n)		(0x205c + (n) * 4)
#define		STATUS0_COMP_RESET_DONE		BIT(0)
#define		STATUS0_COMP_REG_UPDATE0_DONE	BIT(1)
#define		STATUS0_COMP_REG_UPDATE1_DONE	BIT(2)
#define		STATUS0_COMP_REG_UPDATE2_DONE	BIT(3)
#define		STATUS0_COMP_REG_UPDATE3_DONE	BIT(4)
#define		STATUS0_COMP_REG_UPDATE_DONE(n)	BIT((n) + 1)
#define		STATUS0_COMP0_BUF_DONE		BIT(5)
#define		STATUS0_COMP1_BUF_DONE		BIT(6)
#define		STATUS0_COMP2_BUF_DONE		BIT(7)
#define		STATUS0_COMP3_BUF_DONE		BIT(8)
#define		STATUS0_COMP4_BUF_DONE		BIT(9)
#define		STATUS0_COMP5_BUF_DONE		BIT(10)
#define		STATUS0_COMP_BUF_DONE(n)	BIT((n) + 5)
#define		STATUS0_COMP_ERROR		BIT(11)
#define		STATUS0_COMP_OVERWRITE		BIT(12)
#define		STATUS0_OVERFLOW		BIT(13)
#define		STATUS0_VIOLATION		BIT(14)
#define VFE_BUS_ERROR_MASK (STATUS0_COMP_ERROR | STATUS0_COMP_OVERWRITE | \
			    STATUS0_OVERFLOW | STATUS0_VIOLATION)
/* WM_CLIENT_BUF_DONE defined for buffers 0:19 */
#define		STATUS1_WM_CLIENT_BUF_DONE(n)		BIT(n)
#define		STATUS1_EARLY_DONE			BIT(24)
#define		STATUS2_DUAL_COMP0_BUF_DONE		BIT(0)
#define		STATUS2_DUAL_COMP1_BUF_DONE		BIT(1)
#define		STATUS2_DUAL_COMP2_BUF_DONE		BIT(2)
#define		STATUS2_DUAL_COMP3_BUF_DONE		BIT(3)
#define		STATUS2_DUAL_COMP4_BUF_DONE		BIT(4)
#define		STATUS2_DUAL_COMP5_BUF_DONE		BIT(5)
#define		STATUS2_DUAL_COMP_BUF_DONE(n)		BIT(n)
#define		STATUS2_DUAL_COMP_ERROR			BIT(6)
#define		STATUS2_DUAL_COMP_OVERWRITE		BIT(7)

#define VFE_BUS_COMP_GRP_0			(0x2010)
#define VFE_BUS_COMP_ERROR_STATUS		(0x206c)
#define VFE_BUS_COMP_OVERWRITE_STATUS		(0x2070)
#define VFE_BUS_DEBUG_STATUS_0			(0x2270)

#define VFE_BUS_IRQ_CLEAR_GLOBAL		(0x2068)

#define VFE_BUS_WM_DEBUG_STATUS_CFG		(0x226c)
#define		DEBUG_STATUS_CFG_STATUS0(n)	BIT(n)
#define		DEBUG_STATUS_CFG_STATUS1(n)	BIT(8 + (n))

#define VFE_BUS_WM_ADDR_SYNC_FRAME_HEADER	(0x2080)

#define VFE_BUS_WM_ADDR_SYNC_NO_SYNC		(0x2084)
#define		BUS_VER2_MAX_CLIENTS (24)
#define		WM_ADDR_NO_SYNC_DEFAULT_VAL \
				((1 << BUS_VER2_MAX_CLIENTS) - 1)

#define VFE_BUS_WM_CGC_OVERRIDE			(0x200c)
#define		WM_CGC_OVERRIDE_ALL		(0xFFFFF)

#define VFE_BUS_WM_TEST_BUS_CTRL		(0x211c)

#define VFE_BUS_WM_STATUS0(n)			(0x2200 + (n) * 0x100)
#define VFE_BUS_WM_STATUS1(n)			(0x2204 + (n) * 0x100)
#define VFE_BUS_WM_CFG(n)			(0x2208 + (n) * 0x100)
#define		WM_CFG_EN			(0)
#define		WM_CFG_MODE			(1)
#define			MODE_QCOM_PLAIN	(0)
#define			MODE_MIPI_RAW	(1)
#define		WM_CFG_VIRTUALFRAME		(2)
#define VFE_BUS_WM_HEADER_ADDR(n)		(0x220c + (n) * 0x100)
#define VFE_BUS_WM_HEADER_CFG(n)		(0x2210 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_ADDR(n)		(0x2214 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_ADDR_OFFSET(n)		(0x2218 + (n) * 0x100)
#define VFE_BUS_WM_BUFFER_WIDTH_CFG(n)		(0x221c + (n) * 0x100)
#define		WM_BUFFER_DEFAULT_WIDTH		(0xFF01)

#define VFE_BUS_WM_BUFFER_HEIGHT_CFG(n)		(0x2220 + (n) * 0x100)
#define VFE_BUS_WM_PACKER_CFG(n)		(0x2224 + (n) * 0x100)

#define VFE_BUS_WM_STRIDE(n)			(0x2228 + (n) * 0x100)
#define		WM_STRIDE_DEFAULT_STRIDE	(0xFF01)

#define VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(n)	(0x2248 + (n) * 0x100)
#define VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(n)	(0x224c + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PERIOD(n)		(0x2250 + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PATTERN(n)		(0x2254 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_INC(n)			(0x2258 + (n) * 0x100)
#define VFE_BUS_WM_BURST_LIMIT(n)		(0x225c + (n) * 0x100)

static int vfe_pix_pattern(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		return 0;
	case MEDIA_BUS_FMT_SGRBG10_1X10:
		return 1;
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		return 2;
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		return 3;
	default:
		return -EINVAL;
	}
}

static void vfe_pix_wm_geometry(struct vfe_line *line, struct vfe_device *vfe,
				u8 wm, u32 *width, u32 *height, u32 *stride,
				u32 *frame_inc)
{
	struct vfe_output *output = &line->output;
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;

	if (line->dual_ife) {
		u32 left_w = (pix->width / 2) & ~0xfU;

		*width = (vfe == line->slave_vfe) ?
			 (pix->width - left_w) : left_w;
	} else {
		*width = pix->width;
	}
	*height = pix->height;
	if (wm == output->wm_idx[1])
		*height /= 2;
	*stride = pix->plane_fmt[0].bytesperline;
	*frame_inc = *stride * *height;
}

static u32 vfe_scale_phase(u32 input, u32 output)
{
	u8 res = vfe_calc_interp_reso(input, output);

	return (res << 28) | (input << (14 + res)) / output;
}

static void vfe_pix_scale(struct vfe_device *vfe, u32 base,
			  u32 in_width, u32 in_height,
			  u32 out_width, u32 out_height)
{
	writel_relaxed(SCALE_H_EN | SCALE_V_EN, vfe->base + base);
	writel_relaxed(((out_width - 1) << 16) | (in_width - 1),
		       vfe->base + VFE_SCALE_H_SIZE(base));
	writel_relaxed(vfe_scale_phase(in_width, out_width),
		       vfe->base + VFE_SCALE_H_PHASE(base));
	writel_relaxed(0, vfe->base + VFE_SCALE_H_STRIPE(base));
	writel_relaxed(0, vfe->base + VFE_SCALE_H_PHASE_INIT(base));
	writel_relaxed(in_width - 1, vfe->base + VFE_SCALE_H_PAD(base));
	writel_relaxed(((out_height - 1) << 16) | (in_height - 1),
		       vfe->base + VFE_SCALE_V_SIZE(base));
	writel_relaxed(vfe_scale_phase(in_height, out_height),
		       vfe->base + VFE_SCALE_V_PHASE(base));
	writel_relaxed(0, vfe->base + VFE_SCALE_V_STRIPE(base));
	writel_relaxed(0, vfe->base + VFE_SCALE_V_PHASE_INIT(base));
	writel_relaxed(in_height - 1, vfe->base + VFE_SCALE_V_PAD(base));
}

static void vfe_pix_scale_stripe(struct vfe_device *vfe, u32 base,
				 u32 full_in_w, u32 full_in_h,
				 u32 full_out_w, u32 full_out_h,
				 u32 csid_start, u32 stripe_in_w,
				 u32 out_start, u32 out_end)
{
	u8 h_res = vfe_calc_interp_reso(full_in_w, full_out_w);
	u32 h_phase_step = (full_in_w << (14 + h_res)) / full_out_w;
	u64 phase_unit = 1ULL << (14 + h_res);
	u32 last_in = min_t(u32, full_in_w,
			    ((out_end + 1) * full_in_w + full_out_w - 1) /
			    full_out_w) - 1;
	u32 first_in = (out_start * full_in_w) / full_out_w;
	u32 mn_acc = 0, out_cnt = 0, in_idx = 0, extra = 0;
	u64 phase_acc = 0;
	u32 h_skip, pad_end;

	if (out_start > 0) {
		while (true) {
			mn_acc += full_out_w;
			phase_acc += phase_unit;
			if (mn_acc >= full_in_w) {
				out_cnt++;
				mn_acc -= full_in_w;
				phase_acc -= h_phase_step;
				if (phase_acc >= phase_unit)
					phase_acc = phase_unit - 1;
			}
			if (in_idx >= first_in) {
				in_idx++;
				break;
			}
			in_idx++;
			if (out_cnt >= out_start)
				break;
		}
		if (in_idx > first_in && phase_acc < 0x4000)
			extra = 1;
	}

	h_skip = (first_in > csid_start ? first_in - csid_start : 0) + extra;
	pad_end = clamp_t(u32, last_in > csid_start ? last_in - csid_start : 0,
			  h_skip, stripe_in_w - 1);

	writel_relaxed(SCALE_H_EN | SCALE_V_EN, vfe->base + base);
	writel_relaxed(((full_out_w - 1) << 16) | (full_in_w - 1),
		       vfe->base + VFE_SCALE_H_SIZE(base));
	writel_relaxed(((u32)h_res << 28) | (h_phase_step & 0x3fffff),
		       vfe->base + VFE_SCALE_H_PHASE(base));
	writel_relaxed(mn_acc & 0x3fff, vfe->base + VFE_SCALE_H_STRIPE(base));
	writel_relaxed((u32)phase_acc & 0x3fffff,
		       vfe->base + VFE_SCALE_H_PHASE_INIT(base));
	writel_relaxed(BIT(31) | ((h_skip & 0x3fff) << 16) | (pad_end & 0x3fff),
		       vfe->base + VFE_SCALE_H_PAD(base));
	writel_relaxed(((full_out_h - 1) << 16) | (full_in_h - 1),
		       vfe->base + VFE_SCALE_V_SIZE(base));
	writel_relaxed(vfe_scale_phase(full_in_h, full_out_h),
		       vfe->base + VFE_SCALE_V_PHASE(base));
	writel_relaxed(0, vfe->base + VFE_SCALE_V_STRIPE(base));
	writel_relaxed(0, vfe->base + VFE_SCALE_V_PHASE_INIT(base));
	writel_relaxed(full_in_h - 1, vfe->base + VFE_SCALE_V_PAD(base));
}

static struct v4l2_qcom_camss_ife_crop vfe_pix_default_crop(const struct vfe_line *line)
{
	return (struct v4l2_qcom_camss_ife_crop) {
		.compose_width = line->compose.width,
		.compose_height = line->compose.height,
		.left = line->crop.left,
		.top = line->crop.top,
	};
}

static void vfe_pix_write_crop(struct vfe_line *line,
			      const struct v4l2_qcom_camss_ife_crop *crop)
{
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	u32 first = crop->left;
	u32 last = first + line->crop.width - 1;
	u32 top = crop->top;
	u32 bottom = top + line->crop.height - 1;

	if (line->dual_ife && line->slave_vfe) {
		struct vfe_device *slave = line->slave_vfe;
		u32 out_w0 = (line->crop.width / 2) & ~0xfU;
		u32 out_w1 = line->crop.width - out_w0;
		u32 out_x0 = crop->left;
		u32 out_x1 = crop->left + out_w0;

		vfe_pix_scale_stripe(vfe, VFE_FULL_SCALE_Y_CFG,
				     sink->width, sink->height,
				     crop->compose_width, crop->compose_height,
				     0, line->dual_left_in_w,
				     out_x0, out_x1 - 1);
		vfe_pix_scale_stripe(vfe, VFE_FULL_SCALE_C_CFG,
				     sink->width, sink->height,
				     crop->compose_width / 2,
				     crop->compose_height / 2,
				     0, line->dual_left_in_w,
				     out_x0 / 2, out_x1 / 2 - 1);
		writel_relaxed(out_w0 - 1,
			       vfe->base + VFE_FULL_OUT_Y_CROP_PIXEL_CFG);
		writel_relaxed((top << 16) | bottom,
			       vfe->base + VFE_FULL_OUT_Y_CROP_LINE_CFG);
		writel_relaxed(out_w0 - 1,
			       vfe->base + VFE_FULL_OUT_C_CROP_PIXEL_CFG);
		writel_relaxed((top / 2 << 16) | (bottom / 2),
			       vfe->base + VFE_FULL_OUT_C_CROP_LINE_CFG);

		vfe_pix_scale_stripe(slave, VFE_FULL_SCALE_Y_CFG,
				     sink->width, sink->height,
				     crop->compose_width, crop->compose_height,
				     line->dual_right_in_x,
				     line->dual_right_in_w,
				     out_x1, out_x1 + out_w1 - 1);
		vfe_pix_scale_stripe(slave, VFE_FULL_SCALE_C_CFG,
				     sink->width, sink->height,
				     crop->compose_width / 2,
				     crop->compose_height / 2,
				     line->dual_right_in_x,
				     line->dual_right_in_w,
				     out_x1 / 2,
				     (out_x1 + out_w1) / 2 - 1);
		writel_relaxed(out_w1 - 1,
			       slave->base + VFE_FULL_OUT_Y_CROP_PIXEL_CFG);
		writel_relaxed((top << 16) | bottom,
			       slave->base + VFE_FULL_OUT_Y_CROP_LINE_CFG);
		writel_relaxed(out_w1 - 1,
			       slave->base + VFE_FULL_OUT_C_CROP_PIXEL_CFG);
		writel_relaxed((top / 2 << 16) | (bottom / 2),
			       slave->base + VFE_FULL_OUT_C_CROP_LINE_CFG);
		line->ife_active_crop = *crop;
		return;
	}

	vfe_pix_scale(vfe, VFE_FULL_SCALE_Y_CFG, sink->width, sink->height,
		      crop->compose_width, crop->compose_height);
	vfe_pix_scale(vfe, VFE_FULL_SCALE_C_CFG, sink->width, sink->height,
		      crop->compose_width / 2, crop->compose_height / 2);

	writel_relaxed((first << 16) | last,
		       vfe->base + VFE_FULL_OUT_Y_CROP_PIXEL_CFG);
	writel_relaxed((top << 16) | bottom,
		       vfe->base + VFE_FULL_OUT_Y_CROP_LINE_CFG);
	/* Chroma crop counts interleaved components horizontally. */
	writel_relaxed((first << 16) | last,
		       vfe->base + VFE_FULL_OUT_C_CROP_PIXEL_CFG);
	writel_relaxed((top / 2 << 16) | (bottom / 2),
		       vfe->base + VFE_FULL_OUT_C_CROP_LINE_CFG);
	line->ife_active_crop = *crop;
}

static void vfe_ife_write_black_level(struct vfe_device *vfe,
				      const struct v4l2_qcom_camss_ife_config *config)
{
	u32 gain_q11 = min_t(u64,
			     div_u64(1ULL << 25,
				     0x4000 - config->black_level),
			     0x1ffff);
	u32 cfg1 = ((config->black_level_comp[2] & 0x3fff) << 16) |
		   (config->black_level_comp[1] & 0x3fff);
	u32 cfg2 = ((config->black_level_comp[3] & 0x3fff) << 16) |
		   (config->black_level_comp[0] & 0x3fff);

	writel_relaxed((gain_q11 << 15) | (config->black_level & 0x3fff),
		       vfe->base + VFE_BLS_CFG_0);
	writel_relaxed(cfg1,
		       vfe->base + VFE_BLS_CFG_1);
	writel_relaxed(cfg2,
		       vfe->base + VFE_BLS_CFG_2);
}

static void vfe_ife_write_demux_dev(struct vfe_device *vfe,
				    const struct v4l2_qcom_camss_demux *demux,
				    int pattern)
{
	u32 r = demux->gain_q10[0] & 0x7fff;
	u32 gr = demux->gain_q10[1] & 0x7fff;
	u32 gb = demux->gain_q10[2] & 0x7fff;
	u32 b = demux->gain_q10[3] & 0x7fff;
	/* Even line holds Gr for RGGB/GRBG (0/1) and Gb for BGGR/GBRG (2/3). */
	u32 g_even = (pattern < 2) ? gr : gb;
	u32 g_odd = (pattern < 2) ? gb : gr;
	u32 g_reg = (g_odd << 16) | g_even;
	u32 rb_reg = (b << 16) | r;

	writel_relaxed(g_reg, vfe->base + VFE_DEMUX_CFG + 4);
	writel_relaxed(rb_reg, vfe->base + VFE_DEMUX_CFG + 8);
	writel_relaxed(g_reg, vfe->base + VFE_DEMUX_CFG + 12);
	writel_relaxed(rb_reg, vfe->base + VFE_DEMUX_CFG + 16);
}

static void vfe_ife_write_demux(struct vfe_line *line,
				const struct v4l2_qcom_camss_demux *demux,
				int pattern)
{
	vfe_ife_write_demux_dev(to_vfe(line), demux, pattern);
	if (line->dual_ife && line->slave_vfe)
		vfe_ife_write_demux_dev(line->slave_vfe, demux, pattern);
}

static void vfe_ife_write_bpc_wb(struct vfe_device *vfe, const u16 wb_gain_q7[4])
{
	u32 r = max_t(u32, wb_gain_q7[0], 1);
	u32 g = max_t(u32, wb_gain_q7[1], 1);
	u32 b = max_t(u32, wb_gain_q7[3], 1);

	writel_relaxed(clamp_t(u32, DIV_ROUND_CLOSEST(4096U * g, r), 128U, 0x1ffffU),
		       vfe->base + VFE_BPC_CFG_0 + 4);
	writel_relaxed(clamp_t(u32, DIV_ROUND_CLOSEST(4096U * g, b), 128U, 0x1ffffU),
		       vfe->base + VFE_BPC_CFG_0 + 8);
	writel_relaxed(clamp_t(u32, DIV_ROUND_CLOSEST(4096U * r, g), 128U, 0x1ffffU),
		       vfe->base + VFE_BPC_CFG_0 + 12);
	writel_relaxed(clamp_t(u32, DIV_ROUND_CLOSEST(4096U * b, g), 128U, 0x1ffffU),
		       vfe->base + VFE_BPC_CFG_0 + 16);
}

static void vfe_ife_write_bpc_dev(struct vfe_device *vfe,
				  const struct vfe_line *line,
				  const struct v4l2_qcom_camss_bpc *bpc)
{
	writel_relaxed((bpc->enable & 0x7) | ((u32)(bpc->sat_value & 0xfff) << 3),
		       vfe->base + VFE_BPC_CFG_0);
	vfe_ife_write_bpc_wb(vfe, line->ife_wb_gain_q7);
	writel_relaxed(((u32)(bpc->offset_cold & 0x3fff) << 16) |
		       (bpc->offset_hot & 0x3fff),
		       vfe->base + VFE_BPC_CFG_0 + 20);
	writel_relaxed(((u32)(bpc->bcc_offset & 0x1fff) << 16) |
		       ((u32)(bpc->fmin & 0x7f) << 8) |
		       (bpc->fmax & 0x7f),
		       vfe->base + VFE_BPC_CFG_0 + 24);
	writel_relaxed(0, vfe->base + VFE_BPC_CFG_0 + 28);
}

static void vfe_ife_write_bpc(struct vfe_line *line,
			      const struct v4l2_qcom_camss_bpc *bpc)
{
	vfe_ife_write_bpc_dev(to_vfe(line), line, bpc);
	if (line->dual_ife && line->slave_vfe)
		vfe_ife_write_bpc_dev(line->slave_vfe, line, bpc);
}

static void vfe_ife_write_abf_dev(struct vfe_device *vfe,
				  const struct vfe_line *line,
				  const struct v4l2_qcom_camss_abf *abf,
				  u8 bank, u32 stripe_x)
{
	const struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	s32 full_dx = clamp_t(s32, -(s32)(sink->width / 2), -3120, 3110);
	s32 dx = clamp_t(s32, (s32)stripe_x - (s32)(sink->width / 2), -3120, 3110);
	s32 dy = clamp_t(s32, -(s32)(sink->height / 2), -8192, -5);
	u32 full_dist_sq = min_t(u32, (u32)(full_dx * full_dx + dy * dy),
				 0x04948900U);
	u32 dist_sq = min_t(u32, (u32)(dx * dx + dy * dy), 0x04948900U);
	u32 r_sq_shift = 0;
	u32 cgc_override, cfg0;
	unsigned int i;

	while ((full_dist_sq >> r_sq_shift) >= 0x2000 && r_sq_shift < 14)
		r_sq_shift++;

	/* ABF LUT RAM needs its clock even before the lens module is enabled. */
	cgc_override = readl_relaxed(vfe->base + VFE_MODULE_LENS_CGC_OVERRIDE);
	writel(~0U, vfe->base + VFE_MODULE_LENS_CGC_OVERRIDE);

	writel_relaxed(DMI_CFG_AUTO_INCREMENT | (DMI_ABF_BANK_0 + bank),
		       vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
	for (i = 0; i < V4L2_QCOM_CAMSS_ABF_LUT_ENTRIES - 1; i++) {
		u32 base = clamp_t(u32, abf->noise_std_lut[i], 1, 511);
		u32 next = clamp_t(u32, abf->noise_std_lut[i + 1], 1, 511);
		u32 delta = min_t(u32, abs((int)next - (int)base), 511);

		writel_relaxed((delta << 9) | base, vfe->base + VFE_DMI_DATA_LO);
	}
	writel_relaxed(0, vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);

	cfg0 = ((abf->enable & V4L2_QCOM_CAMSS_ABF_ENABLE_FILTER) ? BIT(0) : 0) |
	       ((u32)bank << 1) |
	       ((abf->enable & V4L2_QCOM_CAMSS_ABF_ENABLE_SINGLE_BPC) ? BIT(2) : 0) |
	       ((abf->enable & V4L2_QCOM_CAMSS_ABF_ENABLE_CROSS_PLANE) ? BIT(3) : 0) |
	       ((u32)(abf->bpc_max_shft & 0x7) << 4) |
	       ((u32)(abf->bpc_min_shft & 0x7) << 8) |
	       ((u32)(abf->act_th_r & 0x7) << 12) |
	       ((u32)(abf->act_th_g & 0x3) << 16) |
	       ((u32)(abf->act_th_b & 0x3) << 18) |
	       ((u32)(abf->blk_pix_r & 0x7) << 20) |
	       ((u32)(abf->blk_pix_g & 0x3) << 24) |
	       ((u32)(abf->blk_pix_b & 0x3) << 26);
	writel_relaxed(cfg0, vfe->base + VFE_ABF_CFG_0);

	for (i = 0; i < 4; i++)
		writel_relaxed(abf->spatial_wt[i] & 0x7f,
			       vfe->base + VFE_ABF_CFG_1 + 4 * i);
	writel_relaxed((((u32)(dy & 0x3fff)) << 16) | ((u32)(dx & 0x3fff)),
		       vfe->base + VFE_ABF_CFG_1 + 0x10);
	writel_relaxed(dist_sq & 0x0fffffffU,
		       vfe->base + VFE_ABF_CFG_1 + 0x14);
	writel_relaxed(0x0fff0fffU, vfe->base + VFE_ABF_CFG_1 + 0x18);
	writel_relaxed(0x0fff0fffU, vfe->base + VFE_ABF_CFG_1 + 0x1c);
	for (i = 0; i < 8; i++)
		writel_relaxed(abf->curve_offset[i],
			       vfe->base + VFE_ABF_CFG_1 + 0x20 + 4 * i);
	writel_relaxed(r_sq_shift & 0xf,
		       vfe->base + VFE_ABF_CFG_1 + 0x40);
	writel_relaxed(((u32)(abf->bpc_offset & 0xfff) << 16) |
		       ((u32)(abf->edge_softness[1] & 0x3f) << 8) |
		       (abf->edge_softness[0] & 0x3f),
		       vfe->base + VFE_ABF_CFG_1 + 0x44);
	writel_relaxed(((u32)min_t(u16, line->ife_config.black_level, 0xfff) << 8) |
		       ((u32)(abf->edge_softness[2] & 0xf) << 4) |
		       (abf->edge_softness[3] & 0xf),
		       vfe->base + VFE_ABF_CFG_1 + 0x48);
	writel_relaxed(((u32)(abf->noise_preserve_span & 0x3ff) << 16) |
		       (abf->noise_preserve_lo & 0x3ff),
		       vfe->base + VFE_ABF_CFG_1 + 0x4c);
	writel_relaxed(abf->noise_preserve_base[0] & 0x1ff,
		       vfe->base + VFE_ABF_CFG_1 + 0x50);
	writel_relaxed(abf->noise_preserve_base[1] & 0x1ff,
		       vfe->base + VFE_ABF_CFG_1 + 0x54);

	wmb();
	writel(cgc_override, vfe->base + VFE_MODULE_LENS_CGC_OVERRIDE);
}

static void vfe_ife_write_abf(struct vfe_line *line,
			      const struct v4l2_qcom_camss_abf *abf,
			      bool reset_bank)
{
	u8 bank = reset_bank ? 0 : line->ife_abf_bank ^ 1;

	vfe_ife_write_abf_dev(to_vfe(line), line, abf, bank, 0);
	if (line->dual_ife && line->slave_vfe)
		vfe_ife_write_abf_dev(line->slave_vfe, line, abf, bank,
				      line->dual_right_in_x);
	line->ife_abf_bank = bank;
}

static void vfe_ife_write_gtm_dev(struct vfe_device *vfe,
				  const struct v4l2_qcom_camss_gtm *gtm,
				  u8 bank)
{
	static const u16 x_knots[V4L2_QCOM_CAMSS_GTM_ENTRIES] = {
		0, 2, 4, 7, 13, 21, 31, 45,
		62, 83, 107, 137, 169, 201, 233, 265,
		297, 329, 361, 428, 463, 499, 538, 578,
		621, 665, 712, 760, 811, 863, 919, 976,
		1037, 1098, 1164, 1230, 1300, 1371, 1447, 1524,
		1605, 1687, 1774, 1861, 1954, 2047, 2146, 2245,
		2456, 2679, 2915, 3429, 3999, 4630, 5323, 6083,
		6911, 7812, 8787, 9840, 10975, 12193, 13499, 14894,
		16383
	};
	u32 cgc_override;
	unsigned int i;

	/* GTM RAM needs its clock even before the color module is enabled. */
	cgc_override = readl_relaxed(vfe->base + VFE_MODULE_COLOR_CGC_OVERRIDE);
	writel(~0U, vfe->base + VFE_MODULE_COLOR_CGC_OVERRIDE);

	writel_relaxed(DMI_CFG_AUTO_INCREMENT | (DMI_GTM_BANK_0 + bank),
		       vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
	for (i = 0; i < V4L2_QCOM_CAMSS_GTM_ENTRIES - 1; i++) {
		u32 base = gtm->y_ratio_q12[i] & 0x3ffff;
		s32 dx = x_knots[i + 1] - x_knots[i];
		s32 diff = ((s32)gtm->y_ratio_q12[i + 1] -
			    (s32)gtm->y_ratio_q12[i]) * 256;
		s32 slope = (diff >= 0) ? ((diff + dx / 2) / dx) :
					  ((diff - dx / 2) / dx);

		slope = clamp(slope, -0x2000000, 0x1ffffff);
		/* DATA_LO commits the 64-bit GTM entry and advances DMI_ADDR. */
		writel_relaxed((u32)slope & 0x3ffffff,
			       vfe->base + VFE_DMI_DATA_HI);
		writel_relaxed(base, vfe->base + VFE_DMI_DATA_LO);
	}
	writel_relaxed(0, vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);

	writel_relaxed(bank, vfe->base + VFE_GTM_CFG);
	wmb();
	writel(cgc_override, vfe->base + VFE_MODULE_COLOR_CGC_OVERRIDE);
}

static void vfe_ife_write_gtm(struct vfe_line *line,
			      const struct v4l2_qcom_camss_gtm *gtm,
			      bool reset_bank)
{
	u8 bank = reset_bank ? 0 : line->ife_gtm_bank ^ 1;

	vfe_ife_write_gtm_dev(to_vfe(line), gtm, bank);
	if (line->dual_ife && line->slave_vfe)
		vfe_ife_write_gtm_dev(line->slave_vfe, gtm, bank);
	line->ife_gtm_bank = bank;
}

static int vfe_ife_write_lsc_dev(struct vfe_device *vfe,
				 const struct vfe_line *line,
				 const struct v4l2_qcom_camss_lsc *lsc,
				 u8 bank, u32 stripe_x)
{
	const struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	const u32 mesh_h = V4L2_QCOM_CAMSS_LSC_H_NUM - 1;
	const u32 mesh_v = V4L2_QCOM_CAMSS_LSC_V_NUM - 1;
	u32 hw = sink->width / 2;
	u32 hh = sink->height / 2;
	u32 interp_s = 0, sub_w = 0, sub_h = 0, blk_w = 0, blk_h = 0;
	u32 pad_w = 0, pad_h = 0;
	bool found = false;
	int s;

	if (hw < mesh_h * 9 || hh < mesh_v * 9)
		return -ERANGE;

	for (s = 3; s >= 0; s--) {
		u32 mask = (1U << s) - 1;
		u32 sw = (((hw - 1 + mesh_h) / mesh_h) + mask) >> s;
		u32 sh = (((hh - 1 + mesh_v) / mesh_v) + mask) >> s;
		u32 bw = sw << s;
		u32 bh = sh << s;
		u32 pw = bw * mesh_h - hw;
		u32 ph = bh * mesh_v - hh;
		u32 lx_try = (pw + 1) >> 1;

		if (ph < bh && pw < bw && sh >= 9 && sw >= 9 &&
		    bw >= 18 && bh >= 9 &&
		    bw - lx_try >= 18 &&
		    sw - (lx_try % sw) >= 9) {
			interp_s = s;
			sub_w = sw;
			sub_h = sh;
			blk_w = bw;
			blk_h = bh;
			pad_w = pw;
			pad_h = ph;
			found = true;
			break;
		}
	}
	if (!found) {
		interp_s = 0;
		sub_w = (hw - 1 + mesh_h) / mesh_h;
		sub_h = (hh - 1 + mesh_v) / mesh_v;
		blk_w = sub_w;
		blk_h = sub_h;
		pad_w = blk_w * mesh_h - hw;
		pad_h = blk_h * mesh_v - hh;
	}

	{
		u32 subgrid_w_m1 = clamp_t(u32, sub_w - 1, 8, 2047);
		u32 subgrid_h_m1 = clamp_t(u32, sub_h - 1, 8, 1023);
		u32 block_w_m1 = clamp_t(u32, blk_w - 1, 17, 2047);
		u32 block_h_m1 = clamp_t(u32, blk_h - 1, 8, 1023);
		u32 x_delta = clamp_t(u32, (1U << 20) / (subgrid_w_m1 + 1), 0x200, 0x1c71c);
		u32 y_delta = clamp_t(u32, (1U << 20) / (subgrid_h_m1 + 1), 0x400, 0x1c71c);
		u32 lx = ((pad_w + 1) >> 1) + (stripe_x >> 1);
		u32 ly = (pad_h + 1) >> 1;
		u32 x_sub_idx = lx / (subgrid_w_m1 + 1);
		u32 y_sub_idx = ly / (subgrid_h_m1 + 1);
		u32 x_blk_idx = min_t(u32, x_sub_idx >> interp_s, 63);
		u32 y_blk_idx = min_t(u32, y_sub_idx >> interp_s, 63);
		u32 x_sub_off = x_sub_idx & ((1U << interp_s) - 1);
		u32 y_sub_off = y_sub_idx & ((1U << interp_s) - 1);
		u32 x_pix = lx - x_sub_idx * (subgrid_w_m1 + 1);
		u32 y_pix = ly - y_sub_idx * (subgrid_h_m1 + 1);
		u32 cfg1 = (block_h_m1 << 16) | block_w_m1;
		u32 cfg2 = (interp_s << 30) | (y_delta << 12) | subgrid_h_m1;
		u32 cfg3 = (x_delta << 12) | subgrid_w_m1;
		u32 cfg7 = (((y_delta * y_pix) & 0xfffffU) << 12) |
			   (y_blk_idx << 6) | x_blk_idx;
		u32 cfg8 = (y_sub_off << 28) | (x_sub_off << 24) |
			   ((y_pix & 0xfffU) << 12) | (x_pix & 0xfffU);
		u8 t0 = bank ? DMI_LSC_BANK_1_T0 : DMI_LSC_BANK_0_T0;
		u8 t1 = bank ? DMI_LSC_BANK_1_T1 : DMI_LSC_BANK_0_T1;
		u32 cgc_override;
		unsigned int i;

		/* Rolloff RAM needs its clock even before the lens module is enabled. */
		cgc_override = readl_relaxed(vfe->base + VFE_MODULE_LENS_CGC_OVERRIDE);
		writel(~0U, vfe->base + VFE_MODULE_LENS_CGC_OVERRIDE);

		writel_relaxed(DMI_CFG_AUTO_INCREMENT | t0, vfe->base + VFE_DMI_CFG);
		writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
		for (i = 0; i < V4L2_QCOM_CAMSS_LSC_POINTS; i++)
			writel_relaxed(((u32)(lsc->gr_q10[i] & 0x1fff) << 13) |
				       (lsc->r_q10[i] & 0x1fff),
				       vfe->base + VFE_DMI_DATA_LO);

		writel_relaxed(DMI_CFG_AUTO_INCREMENT | t1, vfe->base + VFE_DMI_CFG);
		writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
		for (i = 0; i < V4L2_QCOM_CAMSS_LSC_POINTS; i++)
			writel_relaxed(((u32)(lsc->gb_q10[i] & 0x1fff) << 13) |
				       (lsc->b_q10[i] & 0x1fff),
				       vfe->base + VFE_DMI_DATA_LO);

		writel_relaxed(0, vfe->base + VFE_DMI_CFG);
		writel_relaxed(0, vfe->base + VFE_DMI_ADDR);

		writel_relaxed(((mesh_v - 1) << 24) | ((mesh_h - 1) << 18) |
			       ((u32)bank << 16),
			       vfe->base + VFE_ROLLOFF_CFG_0);
		writel_relaxed(cfg1, vfe->base + VFE_ROLLOFF_CFG_0 + 4);
		writel_relaxed(cfg2, vfe->base + VFE_ROLLOFF_CFG_0 + 8);
		writel_relaxed(cfg3, vfe->base + VFE_ROLLOFF_CFG_0 + 12);
		writel_relaxed(cfg1, vfe->base + VFE_ROLLOFF_CFG_0 + 16);
		writel_relaxed(cfg2, vfe->base + VFE_ROLLOFF_CFG_0 + 20);
		writel_relaxed(cfg3, vfe->base + VFE_ROLLOFF_CFG_0 + 24);
		writel_relaxed(cfg7, vfe->base + VFE_ROLLOFF_CFG_0 + 28);
		writel_relaxed(cfg8, vfe->base + VFE_ROLLOFF_CFG_0 + 32);
		writel_relaxed(cfg7, vfe->base + VFE_ROLLOFF_CFG_0 + 36);
		writel_relaxed(cfg8, vfe->base + VFE_ROLLOFF_CFG_0 + 40);

		/* Ensure bank selection and table writes complete before restoring CGC. */
		wmb();
		writel(cgc_override, vfe->base + VFE_MODULE_LENS_CGC_OVERRIDE);
	}

	return 0;
}

static int vfe_ife_write_lsc(struct vfe_line *line,
			     const struct v4l2_qcom_camss_lsc *lsc,
			     bool reset_bank)
{
	u8 bank = reset_bank ? 0 : line->ife_lsc_bank ^ 1;
	int ret;

	ret = vfe_ife_write_lsc_dev(to_vfe(line), line, lsc, bank, 0);
	if (ret)
		return ret;
	if (line->dual_ife && line->slave_vfe) {
		ret = vfe_ife_write_lsc_dev(line->slave_vfe, line, lsc, bank,
					    line->dual_right_in_x);
		if (ret)
			return ret;
	}
	line->ife_lsc_bank = bank;
	return 0;
}

static void vfe_ife_write_wb_dev(struct vfe_device *vfe,
				 const struct vfe_line *line,
				 const struct v4l2_qcom_camss_ife_config *config)
{
	u16 r_gain = config->wb_gain_q7[0] & 0xfff;
	u16 g_gain = config->wb_gain_q7[1] & 0xfff;
	u16 b_gain = config->wb_gain_q7[3] & 0xfff;
	u16 r_offset = (u16)config->wb_offset[0] & 0x7fff;
	u16 g_offset = (u16)config->wb_offset[1] & 0x7fff;
	u16 b_offset = (u16)config->wb_offset[3] & 0x7fff;

	writel_relaxed(g_gain | (b_gain << 16),
		       vfe->base + VFE_DEMO_WB_CFG_0);
	writel_relaxed(r_gain,
		       vfe->base + VFE_DEMO_WB_CFG_1);
	writel_relaxed(g_offset | (b_offset << 16),
		       vfe->base + VFE_DEMO_WB_OFFSET_CFG_0);
	writel_relaxed(r_offset,
		       vfe->base + VFE_DEMO_WB_OFFSET_CFG_1);
	if (line->ife_bpc.enable)
		vfe_ife_write_bpc_wb(vfe, line->ife_wb_gain_q7);
}

static void vfe_ife_write_white_balance(struct vfe_line *line,
					const struct v4l2_qcom_camss_ife_config *config)
{
	memcpy(line->ife_wb_gain_q7, config->wb_gain_q7,
	       sizeof(line->ife_wb_gain_q7));
	vfe_ife_write_wb_dev(to_vfe(line), line, config);
	if (line->dual_ife && line->slave_vfe)
		vfe_ife_write_wb_dev(line->slave_vfe, line, config);
}

static void vfe_pix_write_cst_dev(struct vfe_device *vfe,
				  const struct vfe_line *line)
{
	const struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;
	bool bt709 = pix->ycbcr_enc == V4L2_YCBCR_ENC_709;
	bool limited = pix->quantization == V4L2_QUANTIZATION_LIM_RANGE;
	static const struct {
		s16 r, g, b;
		u16 out_off;
		u16 clamp_min;
		u16 clamp_max;
	} cst[4][3] = {
		/* [0] BT.601 Full Range (sYCC) */
		{
			{  306,  601,  117,   0,  0, 1023 },
			{ -172, -338,  510, 512,  0, 1023 },
			{  510, -427,  -83, 512,  0, 1023 },
		},
		/* [1] BT.601 Limited Range */
		{
			{  263,  516,  100,  64, 64,  940 },
			{ -152, -298,  450, 512, 64,  960 },
			{  450, -377,  -73, 512, 64,  960 },
		},
		/* [2] BT.709 Full Range */
		{
			{  218,  732,   74,   0,  0, 1023 },
			{ -117, -393,  510, 512,  0, 1023 },
			{  510, -463,  -47, 512,  0, 1023 },
		},
		/* [3] BT.709 Limited Range */
		{
			{  187,  629,   63,  64, 64,  940 },
			{ -103, -347,  450, 512, 64,  960 },
			{  450, -409,  -41, 512, 64,  960 },
		},
	};
	unsigned int idx = (bt709 ? 2 : 0) | (limited ? 1 : 0);
	unsigned int row;

	for (row = 0; row < 3; row++) {
		u32 base = VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + row * 16;

		writel_relaxed((((u32)(cst[idx][row].b & 0x1fff)) << 16) |
			       ((u32)(cst[idx][row].g & 0x1fff)),
			       vfe->base + base);
		writel_relaxed((u32)(cst[idx][row].r & 0x1fff),
			       vfe->base + base + 4);
		writel_relaxed((u32)cst[idx][row].out_off << 16,
			       vfe->base + base + 8);
		writel_relaxed(((u32)cst[idx][row].clamp_max << 16) |
			       cst[idx][row].clamp_min,
			       vfe->base + base + 12);
	}

	if (limited) {
		writel_relaxed(FULL_OUT_CLAMP_MAX(235) | FULL_OUT_CLAMP_MIN(16),
			       vfe->base + VFE_FULL_OUT_Y_CH0_CLAMP_CFG);
		writel_relaxed(FULL_OUT_CLAMP_MAX(240) | FULL_OUT_CLAMP_MIN(16),
			       vfe->base + VFE_FULL_OUT_C_CH0_CLAMP_CFG);
	} else {
		writel_relaxed(FULL_OUT_CLAMP_MAX(255) | FULL_OUT_CLAMP_MIN(0),
			       vfe->base + VFE_FULL_OUT_Y_CH0_CLAMP_CFG);
		writel_relaxed(FULL_OUT_CLAMP_MAX(255) | FULL_OUT_CLAMP_MIN(0),
			       vfe->base + VFE_FULL_OUT_C_CH0_CLAMP_CFG);
	}
}

static void vfe_pix_write_cst(struct vfe_line *line)
{
	vfe_pix_write_cst_dev(to_vfe(line), line);
	if (line->dual_ife && line->slave_vfe)
		vfe_pix_write_cst_dev(line->slave_vfe, line);
}

static void vfe_ife_write_ccm(struct vfe_device *vfe,
			      const struct v4l2_qcom_camss_ife_config *config)
{
	static const u8 channels[] = { 1, 2, 0 };
	unsigned int row, col;

	/* CC12 operates in G, B, R order; the payload matrix uses R, G, B. */
	for (row = 0; row < ARRAY_SIZE(channels); row++) {
		for (col = 0; col < ARRAY_SIZE(channels); col++) {
			u16 coeff = config->ccm_q7[3 * channels[row] + channels[col]];

			writel_relaxed(coeff & 0xfff,
				       vfe->base + VFE_COLOR_CORRECT_COEFF_0 +
				       4 * (3 * row + col));
		}
		writel_relaxed((u16)config->ccm_offset[channels[row]] & 0x7ff,
			       vfe->base + VFE_COLOR_CORRECT_OFFSET_0 + 4 * row);
	}
	writel_relaxed(0, vfe->base + VFE_COLOR_CORRECT_COEFF_Q);
}

static void vfe_ife_write_gamma_dev(struct vfe_device *vfe,
				    const struct v4l2_qcom_camss_ife_config *config,
				    u8 bank)
{
	static const struct {
		u8 dmi_bank;
		u8 payload_channel;
	} channels[] = {
		{ DMI_GAMMA_G_BANK_0, 1 },
		{ DMI_GAMMA_B_BANK_0, 2 },
		{ DMI_GAMMA_R_BANK_0, 0 },
	};
	u32 cgc_override;
	unsigned int channel, entry;

	/* Gamma RAM needs its clock even before the color modules are enabled. */
	cgc_override = readl_relaxed(vfe->base + VFE_MODULE_COLOR_CGC_OVERRIDE);
	writel(~0U, vfe->base + VFE_MODULE_COLOR_CGC_OVERRIDE);

	for (channel = 0; channel < ARRAY_SIZE(channels); channel++) {
		const u16 *curve = config->gamma[channels[channel].payload_channel];

		writel_relaxed(DMI_CFG_AUTO_INCREMENT |
			       (channels[channel].dmi_bank + bank),
			       vfe->base + VFE_DMI_CFG);
		writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
		for (entry = 0; entry < V4L2_QCOM_CAMSS_IFE_GAMMA_ENTRIES - 1;
		     entry++) {
			unsigned int base = curve[entry];
			int delta = (int)curve[entry + 1] - (int)base;

			delta = clamp(delta, -512, 511);
			writel_relaxed(base | ((delta & 0x3ff) << 10),
				       vfe->base + VFE_DMI_DATA_LO);
		}
	}
	writel_relaxed(0, vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);

	/* Bits [2:0] select the read bank for B, G, and R channels. */
	writel_relaxed(bank ? GENMASK(2, 0) : 0, vfe->base + VFE_RGB_LUT_CFG);
	/* Ensure LUT bank selection is visible before restoring clock gating. */
	wmb();
	writel(cgc_override, vfe->base + VFE_MODULE_COLOR_CGC_OVERRIDE);
}

static void vfe_ife_write_gamma(struct vfe_line *line,
				const struct v4l2_qcom_camss_ife_config *config,
				bool reset_bank)
{
	u8 bank = reset_bank ? 0 : line->ife_gamma_bank ^ 1;

	vfe_ife_write_gamma_dev(to_vfe(line), config, bank);
	if (line->dual_ife && line->slave_vfe)
		vfe_ife_write_gamma_dev(line->slave_vfe, config, bank);
	line->ife_gamma_bank = bank;
}

static void vfe_ife_write_config(struct vfe_line *line,
				 const struct v4l2_qcom_camss_ife_config *config,
				 u32 flags, bool reset_gamma_bank)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_device *slave = line->dual_ife ? line->slave_vfe : NULL;

	if (flags & V4L2_QCOM_CAMSS_IFE_CONFIG_BLACK_LEVEL) {
		vfe_ife_write_black_level(vfe, config);
		if (slave)
			vfe_ife_write_black_level(slave, config);
	}
	if (flags & V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE)
		vfe_ife_write_white_balance(line, config);
	if (flags & V4L2_QCOM_CAMSS_IFE_CONFIG_CCM) {
		vfe_ife_write_ccm(vfe, config);
		if (slave)
			vfe_ife_write_ccm(slave, config);
	}
	if (flags & V4L2_QCOM_CAMSS_IFE_CONFIG_GAMMA)
		vfe_ife_write_gamma(line, config, reset_gamma_bank);
}

static int vfe_ife_config_validate(const struct v4l2_qcom_camss_ife_config *config)
{
	unsigned int i;

	if (config->version != V4L2_QCOM_CAMSS_IFE_CONFIG_VERSION ||
	    !config->flags ||
	    config->flags & ~V4L2_QCOM_CAMSS_IFE_CONFIG_ALL ||
	    (config->sensor_delay && config->sensor_delay != 2) || config->reserved1 ||
	    config->frame_duration_us || config->sensor_timestamp)
		return -EINVAL;

	if (config->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_BLACK_LEVEL) {
		if (config->black_level > 0x3fff)
			return -ERANGE;
		for (i = 0; i < ARRAY_SIZE(config->black_level_comp); i++)
			if (config->black_level_comp[i] > 0x3fff)
				return -ERANGE;
	}

	if (config->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE) {
		if (config->wb_gain_q7[1] != config->wb_gain_q7[2] ||
		    config->wb_offset[1] != config->wb_offset[2])
			return -EINVAL;
		for (i = 0; i < ARRAY_SIZE(config->wb_gain_q7); i++)
			if (config->wb_gain_q7[i] > 0xfff)
				return -ERANGE;
		for (i = 0; i < ARRAY_SIZE(config->wb_offset); i++)
			if (config->wb_offset[i] < -16384 ||
			    config->wb_offset[i] > 16383)
				return -ERANGE;
	}

	if (config->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_CCM) {
		for (i = 0; i < ARRAY_SIZE(config->ccm_q7); i++)
			if (config->ccm_q7[i] < -2048 ||
			    config->ccm_q7[i] > 2047)
				return -ERANGE;
		for (i = 0; i < ARRAY_SIZE(config->ccm_offset); i++)
			if (config->ccm_offset[i] < -1024 ||
			    config->ccm_offset[i] > 1023)
				return -ERANGE;
	}

	if (config->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_GAMMA)
		for (i = 0; i < ARRAY_SIZE(config->gamma); i++) {
			unsigned int entry;

			for (entry = 0; entry < ARRAY_SIZE(config->gamma[i]); entry++)
				if (config->gamma[i][entry] > 0x3ff)
					return -ERANGE;
		}

	return 0;
}

static int vfe_ife_config_try_ctrl(struct v4l2_ctrl *ctrl)
{
	return vfe_ife_config_validate(ctrl->p_new.p);
}

static int vfe_ife_config_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_qcom_camss_ife_config *request = ctrl->p_new.p;
	struct v4l2_qcom_camss_ife_config config;
	unsigned long irq_flags;
	bool streaming;
	bool update_config = false;
	int ret = 0;

	if (request->sensor_delay)
		return -EINVAL;

	mutex_lock(&line->ife_config_lock);
	config = line->ife_config;
	config.frame_sequence = request->frame_sequence;
	config.flags = V4L2_QCOM_CAMSS_IFE_CONFIG_ALL;

	if (request->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_BLACK_LEVEL) {
		config.black_level = request->black_level;
		memcpy(config.black_level_comp, request->black_level_comp,
		       sizeof(config.black_level_comp));
	}
	if (request->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE) {
		memcpy(config.wb_gain_q7, request->wb_gain_q7,
		       sizeof(config.wb_gain_q7));
		memcpy(config.wb_offset, request->wb_offset,
		       sizeof(config.wb_offset));
	}
	if (request->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_CCM) {
		memcpy(config.ccm_q7, request->ccm_q7, sizeof(config.ccm_q7));
		memcpy(config.ccm_offset, request->ccm_offset,
		       sizeof(config.ccm_offset));
	}
	if (request->flags & V4L2_QCOM_CAMSS_IFE_CONFIG_GAMMA)
		memcpy(config.gamma, request->gamma, sizeof(config.gamma));

	spin_lock_irqsave(&vfe->output_lock, irq_flags);
	streaming = line->output.state == VFE_OUTPUT_ON;
	if (streaming && line->video_out.vb2_q.uses_requests) {
		ret = -EBUSY;
	} else if (!streaming && request->frame_sequence) {
		ret = -EINVAL;
	} else if (streaming && request->frame_sequence &&
		   request->frame_sequence != line->output.sof_sequence) {
		ret = -EAGAIN;
	} else if (streaming) {
		vfe_ife_write_config(line, &config, request->flags, false);
		vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
		update_config = true;
	} else {
		update_config = true;
	}
	spin_unlock_irqrestore(&vfe->output_lock, irq_flags);
	if (update_config)
		line->ife_config = config;
	mutex_unlock(&line->ife_config_lock);

	return ret;
}

static void vfe_sensor_record(struct vfe_line *line, u32 sequence,
			      struct camss_buffer *buf, bool valid)
{
	struct vfe_sensor_state *state;

	state = &line->sensor_history[line->sensor_count++ % ARRAY_SIZE(line->sensor_history)];
	*state = (struct vfe_sensor_state) {
		.sequence = sequence,
		.exposure = buf->sensor_exposure,
		.gain = buf->sensor_gain,
		.valid = valid,
	};
}

bool vfe_sensor_state(struct vfe_line *line, u32 sequence, struct vfe_sensor_state *state)
{
	struct vfe_device *vfe = to_vfe(line);
	u32 i, count;
	bool found = false;

	lockdep_assert_held(&vfe->output_lock);
	count = min_t(u32, line->sensor_count, ARRAY_SIZE(line->sensor_history));
	for (i = 0; i < count; i++) {
		*state = line->sensor_history[(line->sensor_count - i - 1) %
					      ARRAY_SIZE(line->sensor_history)];
		if (state->sequence <= sequence) {
			found = state->valid;
			break;
		}
	}
	return found;
}

static void vfe_apply_sensor(struct camss_video *video, struct camss_buffer *buf)
{
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);
	struct vfe_device *vfe = to_vfe(line);
	unsigned long flags;
	u32 sequence, start;
	int ret;

	spin_lock_irqsave(&vfe->output_lock, flags);
	start = line->output.sof_sequence - 1;
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	ret = v4l2_ctrl_request_setup(buf->vb.vb2_buf.request, buf->sensor_controls);
	spin_lock_irqsave(&vfe->output_lock, flags);
	sequence = line->output.sof_sequence - 1;
	/* Queue DMA only after the completed batch has a known future input. */
	buf->input_sequence = sequence + buf->ife.sensor_delay;
	buf->sensor_applied = !ret;
	if (sequence != start)
		vfe_sensor_record(line, start + buf->ife.sensor_delay, buf, false);
	vfe_sensor_record(line, buf->input_sequence, buf, !ret);
	line->sensor_next = sequence + 1;
	if (!ret)
		line->sensor_vblank = buf->sensor_vblank;
	line->sensor_inflight = NULL;
	if (!ret)
		vfe_buf_add_pending(&line->output, buf);
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	if (ret) {
		dev_err(video->camss->dev, "Sensor request programming failed: %d\n", ret);
		camss_buffer_done(buf, VB2_BUF_STATE_ERROR);
	}
}

static void vfe_sensor_schedule(struct vfe_line *line)
{
	struct camss_video *video = &line->video_out;
	struct camss_buffer *buf;

	if (!line->output.sof_sequence || line->sensor_inflight ||
	    line->output.sof_sequence - 1 < line->sensor_next ||
	    list_empty(&line->sensor_pending) || READ_ONCE(video->requests_stopping))
		return;
	buf = list_first_entry(&line->sensor_pending, struct camss_buffer, queue);
	/*
	 * Keep the next frame-length change out of the last request's
	 * exposure window. VTS may latch a frame before exposure and gain.
	 */
	if (buf->sensor_vblank != line->sensor_vblank &&
	    line->output.sof_sequence - 1 == line->sensor_next)
		return;
	list_del(&buf->queue);
	line->sensor_inflight = buf;
	queue_work(video->request_wq, &buf->sensor_work);
}

static int vfe_prepare_request(struct camss_video *video, struct camss_buffer *buf)
{
	struct media_pad *pad = camss_find_sensor_pad(&video->vdev.entity);
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);
	const struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	const struct v4l2_qcom_camss_ife_crop *crop = &buf->ife_crop;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl_ref *ref;
	unsigned int controls = 0;
	int ret;

	if (video->active_fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
	    video->active_fmt.fmt.pix_mp.num_planes != 1 || buf->ife.frame_sequence ||
	    buf->ife.sensor_delay != 2 ||
	    (buf->ife.flags & (V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE |
			       V4L2_QCOM_CAMSS_IFE_CONFIG_CCM)) !=
	    (V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE |
	     V4L2_QCOM_CAMSS_IFE_CONFIG_CCM))
		return -EINVAL;
	ret = vfe_ife_config_validate(&buf->ife);
	if (ret)
		return ret;
	if (buf->ife_crop_valid &&
	    (((crop->compose_width | crop->compose_height | crop->left | crop->top) & 1) ||
	     crop->compose_width < line->compose.width ||
	     crop->compose_height < line->compose.height ||
	     crop->compose_width > sink->width || crop->compose_height > sink->height ||
	     crop->left + line->crop.width > crop->compose_width ||
	     crop->top + line->crop.height > crop->compose_height))
		return -ERANGE;
	if (!pad)
		return -ENOLINK;
	buf->sensor_controls = media_entity_to_v4l2_subdev(pad->entity)->ctrl_handler;
	handler = v4l2_ctrl_request_hdl_find(buf->vb.vb2_buf.request, buf->sensor_controls);
	if (!handler)
		return -EINVAL;
	ret = 0;
	mutex_lock(handler->lock);
	list_for_each_entry(ref, &handler->ctrl_refs, node) {
		if (!ref->p_req_valid)
			continue;
		if (ref->ctrl->id == V4L2_CID_EXPOSURE)
			buf->sensor_exposure = *ref->p_req.p_s32;
		else if (ref->ctrl->id == V4L2_CID_ANALOGUE_GAIN)
			buf->sensor_gain = *ref->p_req.p_s32;
		else if (ref->ctrl->id == V4L2_CID_VBLANK)
			buf->sensor_vblank = *ref->p_req.p_s32;
		else
			ret = -EINVAL;
		controls++;
	}
	mutex_unlock(handler->lock);
	v4l2_ctrl_request_hdl_put(handler);
	return controls == 3 ? ret : -EINVAL;
}

static int vfe_start_requests(struct camss_video *video)
{
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);
	const struct v4l2_pix_format_mplane *format = &video->active_fmt.fmt.pix_mp;
	struct vfe_output *output = &line->output;
	u32 sequence = video->recovering ? output->sof_sequence : 0;
	struct camss_buffer *buf, *first;
	unsigned int i, dummy = 0;
	int ret;

	line->sensor_count = 0;
	if (!video->vb2_q.uses_requests ||
	    format->pixelformat != V4L2_PIX_FMT_NV12 || format->num_planes != 1)
		return 0;
	first = list_first_entry_or_null(&output->pending_bufs, struct camss_buffer, queue);
	if (!first && !video->recovering)
		return -EINVAL;
	line->dummy_size = format->plane_fmt[0].sizeimage;
	line->dummy_memory = dma_alloc_coherent(video->camss->dev, line->dummy_size,
						&line->dummy_buffer[0].addr[0], GFP_KERNEL);
	if (!line->dummy_memory)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(line->dummy_buffer); i++) {
		buf = &line->dummy_buffer[i];
		buf->dummy = true;
		buf->addr[0] = line->dummy_buffer[0].addr[0];
		buf->addr[1] = buf->addr[0] + format->plane_fmt[0].bytesperline * format->height;
	}
	if (first) {
		ret = v4l2_ctrl_request_setup(first->vb.vb2_buf.request, first->sensor_controls);
		if (ret) {
			dma_free_coherent(video->camss->dev, line->dummy_size,
					  line->dummy_memory, line->dummy_buffer[0].addr[0]);
			line->dummy_memory = NULL;
			return ret;
		}
		first->input_sequence = sequence;
		first->sensor_applied = true;
		vfe_sensor_record(line, sequence, first, true);
		line->sensor_vblank = first->sensor_vblank;
	} else {
		line->dummy_buffer[dummy].input_sequence = sequence;
		vfe_buf_add_pending(output, &line->dummy_buffer[dummy++]);
	}
	line->sensor_next = sequence;
	line->dummy_buffer[dummy].input_sequence = sequence + 1;
	vfe_buf_add_pending(output, &line->dummy_buffer[dummy]);
	return 0;
}

static void vfe_stop_requests(struct camss_video *video)
{
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);

	line->sensor_inflight = NULL;

	if (line->dummy_memory) {
		dma_free_coherent(video->camss->dev, line->dummy_size,
				  line->dummy_memory, line->dummy_buffer[0].addr[0]);
		line->dummy_memory = NULL;
	}
}

static void vfe_requeue_buffers(struct vfe_line *line)
{
	struct camss_video *video = &line->video_out;
	struct vfe_output *output = &line->output;
	struct camss_buffer *buf, *next;
	int i;

	/* No write master still owns these addresses after the reset. */
	for (i = ARRAY_SIZE(output->buf) - 1; i >= 0; i--) {
		buf = output->buf[i];
		if (buf && !buf->dummy)
			list_add(&buf->queue, &output->pending_bufs);
		output->buf[i] = NULL;
	}
	if (!video->vb2_q.uses_requests)
		return;

	list_for_each_entry_safe(buf, next, &output->pending_bufs, queue) {
		if (buf->dummy) {
			list_del(&buf->queue);
			continue;
		}
		buf->sensor_applied = false;
		buf->ife_applied = false;
		buf->ife.sensor_timestamp = 0;
		buf->ife.frame_duration_us = 0;
	}
	buf = vfe_buf_get_pending(output);
	list_splice_init(&output->pending_bufs, &line->sensor_pending);
	if (buf)
		vfe_buf_add_pending(output, buf);
	vfe_stop_requests(video);
}

static int vfe_stop_streams(struct vfe_device *vfe, unsigned long *active)
{
	int i, ret = 0;

	for (i = 0; i < vfe->res->line_num; i++) {
		struct camss_video *video = &vfe->line[i].video_out;
		int error;

		if (!video->pipeline_running)
			continue;
		__set_bit(i, active);
		error = camss_video_stop_pipeline(video);
		if (error && !ret)
			ret = error;
	}
	return ret;
}

static int vfe_restart_streams(struct vfe_device *vfe, unsigned long active)
{
	int i, ret;

	/* The PIX request must program the sensor before the first CSID port starts. */
	for (i = vfe->res->line_num - 1; i >= 0; i--) {
		struct vfe_line *line = &vfe->line[i];
		struct camss_video *video = &line->video_out;

		if (!test_bit(i, &active))
			continue;
		vfe_requeue_buffers(line);
		video->recovering = true;
		ret = camss_video_start_pipeline(video);
		video->recovering = false;
		if (ret)
			return ret;
	}
	return 0;
}

static void vfe_fail_streams(struct vfe_device *vfe, unsigned long active)
{
	unsigned long flags;
	unsigned int i;

	for_each_set_bit(i, &active, vfe->res->line_num) {
		struct camss_video *video = &vfe->line[i].video_out;

		camss_video_stop_pipeline(video);
		vb2_queue_error(&video->vb2_q);
		camss_video_flush_pipeline(video, VB2_BUF_STATE_ERROR);
	}
	spin_lock_irqsave(&vfe->output_lock, flags);
	vfe->fault_pending = false;
	vfe->recovery_attempted = false;
	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

static void vfe_recover(struct work_struct *work)
{
	struct vfe_device *vfe = container_of(work, struct vfe_device, recover_work);
	struct camss *camss = vfe->camss;
	unsigned long active = 0, flags;
	bool attempted;
	int ret;

	mutex_lock(&camss->stream_lock);
	spin_lock_irqsave(&vfe->output_lock, flags);
	if (!vfe->fault_pending || vfe->recovery_disabled) {
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		goto unlock;
	}
	attempted = vfe->recovery_attempted;
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	/* Keep the configured links stable when the last reference is dropped. */
	mutex_lock(&camss->media_dev.graph_mutex);
	ret = vfe_stop_streams(vfe, &active);
	if (attempted && !ret)
		ret = -EIO;
	if (!ret && active) {
		spin_lock_irqsave(&vfe->output_lock, flags);
		vfe->recovery_attempted = true;
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		ret = vfe_restart_streams(vfe, active);
	}
	if (ret) {
		vfe_fail_streams(vfe, active);
		dev_err(camss->dev, "VFE%u recovery failed: %d\n", vfe->id, ret);
	}
	mutex_unlock(&camss->media_dev.graph_mutex);
unlock:
	mutex_unlock(&camss->stream_lock);
}

void msm_vfe_17x_suspend(struct vfe_device *vfe)
{
	struct camss *camss = vfe->camss;
	unsigned long active = 0;
	unsigned int i;
	int ret;

	mutex_lock(&camss->stream_lock);
	mutex_lock(&camss->media_dev.graph_mutex);
	ret = vfe_stop_streams(vfe, &active);
	if (ret)
		vfe_fail_streams(vfe, active);
	mutex_unlock(&camss->media_dev.graph_mutex);

	for_each_set_bit(i, &active, vfe->res->line_num)
		camss_video_power_put(&vfe->line[i].video_out);
	vfe->suspended_streams = ret ? 0 : active;
	mutex_unlock(&camss->stream_lock);
}

void msm_vfe_17x_resume(struct vfe_device *vfe)
{
	struct camss *camss = vfe->camss;
	unsigned long active;
	unsigned int i;
	int ret = 0;

	mutex_lock(&camss->stream_lock);
	active = vfe->suspended_streams;
	if (!active)
		goto unlock;

	for_each_set_bit(i, &active, vfe->res->line_num) {
		ret = camss_video_power_get(&vfe->line[i].video_out);
		if (ret)
			break;
	}
	mutex_lock(&camss->media_dev.graph_mutex);
	if (!ret)
		ret = vfe_restart_streams(vfe, active);
	if (ret) {
		vfe_fail_streams(vfe, active);
		dev_err(camss->dev, "VFE%u resume failed: %d\n", vfe->id, ret);
	}
	mutex_unlock(&camss->media_dev.graph_mutex);
	if (ret)
		for_each_set_bit(i, &active, vfe->res->line_num)
			camss_video_power_put(&vfe->line[i].video_out);
	vfe->suspended_streams = 0;
unlock:
	mutex_unlock(&camss->stream_lock);
}

static void vfe_ife_apply_request(struct vfe_line *line, struct camss_buffer *buf)
{
	struct vfe_device *vfe = to_vfe(line);
	struct v4l2_qcom_camss_ife_crop crop = buf->ife_crop_valid ?
		buf->ife_crop : vfe_pix_default_crop(line);

	buf->ife.frame_sequence = buf->input_sequence;
	vfe_ife_write_config(line, &buf->ife, buf->ife.flags, false);
	if (memcmp(&crop, &line->ife_active_crop, sizeof(crop)))
		vfe_pix_write_crop(line, &crop);
	vfe->res->hw_ops->reg_update(vfe, line->id);
	buf->ife_applied = true;
}

static void vfe_ife_schedule_request(struct vfe_line *line)
{
	struct vfe_output *output = &line->output;
	struct camss_buffer *buf;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(output->buf); i++) {
		buf = output->buf[i];
		if (buf && buf->ife_valid && buf->input_sequence == output->sof_sequence) {
			vfe_ife_apply_request(line, buf);
			return;
		}
	}
	list_for_each_entry(buf, &output->pending_bufs, queue) {
		if ((s32)(buf->input_sequence - output->sof_sequence) < 0)
			continue;
		if (buf->ife_valid && buf->input_sequence == output->sof_sequence)
			vfe_ife_apply_request(line, buf);
		break;
	}
}

static int vfe_queue_request_buffer(struct camss_video *video, struct camss_buffer *buf)
{
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	unsigned long flags;

	if (!buf->ife_valid)
		return vfe_queue_buffer_v2(video, buf);
	spin_lock_irqsave(&vfe->output_lock, flags);
	if (output->state != VFE_OUTPUT_ON && list_empty(&output->pending_bufs)) {
		buf->input_sequence = 0;
		vfe_buf_add_pending(output, buf);
	} else {
		list_add_tail(&buf->queue, &line->sensor_pending);
	}
	if (output->state == VFE_OUTPUT_ON)
		vfe_sensor_schedule(line);
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	return 0;
}

static struct camss_buffer *vfe_next_request_buffer(struct vfe_line *line, u32 sequence)
{
	struct vfe_output *output = &line->output;
	struct camss_buffer *buf;
	unsigned int i;

	while (!list_empty(&output->pending_bufs)) {
		buf = list_first_entry(&output->pending_bufs, struct camss_buffer, queue);
		if ((s32)(buf->input_sequence - sequence) > 0)
			break;
		buf = vfe_buf_get_pending(output);
		if (buf->input_sequence == sequence)
			return buf;
		camss_buffer_done(buf, VB2_BUF_STATE_ERROR);
	}
	/* Two active addresses keep the DMA input phase defined through queue gaps. */
	for (i = 0; i < ARRAY_SIZE(line->dummy_buffer); i++) {
		buf = &line->dummy_buffer[i];
		if (buf != output->buf[0] && buf != output->buf[1]) {
			buf->input_sequence = sequence;
			return buf;
		}
	}
	return NULL;
}

static const struct v4l2_ctrl_ops vfe_ife_ctrl_ops = {
	.try_ctrl = vfe_ife_config_try_ctrl,
	.s_ctrl = vfe_ife_config_s_ctrl,
};

static int vfe_ife_crop_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_ife_crop *crop = ctrl->p_new.p;

	if (!crop->compose_width || !crop->compose_height ||
	    ((crop->compose_width | crop->compose_height | crop->left | crop->top) & 1))
		return -ERANGE;

	return 0;
}

static int vfe_ife_crop_s_ctrl(struct v4l2_ctrl *ctrl)
{
	return -EINVAL;
}

static const struct v4l2_ctrl_ops vfe_ife_crop_ctrl_ops = {
	.try_ctrl = vfe_ife_crop_try_ctrl,
	.s_ctrl = vfe_ife_crop_s_ctrl,
};

static int vfe_bf_roi_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_bf_roi *roi = ctrl->p_new.p;

	if (!roi->left && !roi->top && !roi->width && !roi->height)
		return 0;
	if (((roi->left | roi->top | roi->width | roi->height) & 1) ||
	    roi->left < 64 || roi->top < 32 ||
	    roi->width < 80 || roi->height < 40)
		return -ERANGE;

	return 0;
}

static int vfe_bf_roi_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;

	return vfe_17x_stats_set_bf_roi(line, ctrl->p_new.p);
}

static const struct v4l2_ctrl_ops vfe_bf_roi_ctrl_ops = {
	.try_ctrl = vfe_bf_roi_try_ctrl,
	.s_ctrl = vfe_bf_roi_s_ctrl,
};

static int vfe_lsc_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_lsc *lsc = ctrl->p_new.p;
	unsigned int i;

	if (lsc->enable > 1)
		return -EINVAL;
	if (!lsc->enable)
		return 0;

	for (i = 0; i < V4L2_QCOM_CAMSS_LSC_POINTS; i++) {
		if (lsc->r_q10[i] < 1024 || lsc->r_q10[i] > 8191 ||
		    lsc->gr_q10[i] < 1024 || lsc->gr_q10[i] > 8191 ||
		    lsc->gb_q10[i] < 1024 || lsc->gb_q10[i] > 8191 ||
		    lsc->b_q10[i] < 1024 || lsc->b_q10[i] > 8191)
			return -ERANGE;
	}
	return 0;
}

static int vfe_lsc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_qcom_camss_lsc *req = ctrl->p_new.p;
	unsigned long irq_flags;
	int ret = 0;

	mutex_lock(&line->ife_config_lock);
	spin_lock_irqsave(&vfe->output_lock, irq_flags);
	if (line->output.state == VFE_OUTPUT_ON) {
		if (req->enable) {
			ret = vfe_ife_write_lsc(line, req, false);
			if (!ret) {
				line->ife_lens_en |= LENS_ROLLOFF_EN;
				writel_relaxed(line->ife_lens_en,
					       vfe->base + VFE_MODULE_LENS_EN);
				if (line->dual_ife && line->slave_vfe)
					writel_relaxed(line->ife_lens_en,
						       line->slave_vfe->base +
						       VFE_MODULE_LENS_EN);
				vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
			}
		} else {
			line->ife_lens_en &= ~LENS_ROLLOFF_EN;
			writel_relaxed(line->ife_lens_en,
				       vfe->base + VFE_MODULE_LENS_EN);
			if (line->dual_ife && line->slave_vfe)
				writel_relaxed(line->ife_lens_en,
					       line->slave_vfe->base +
					       VFE_MODULE_LENS_EN);
			vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
		}
	}
	if (!ret)
		line->ife_lsc = *req;
	spin_unlock_irqrestore(&vfe->output_lock, irq_flags);
	mutex_unlock(&line->ife_config_lock);

	return ret;
}

static const struct v4l2_ctrl_ops vfe_lsc_ctrl_ops = {
	.try_ctrl = vfe_lsc_try_ctrl,
	.s_ctrl = vfe_lsc_s_ctrl,
};

static int vfe_demux_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_demux *demux = ctrl->p_new.p;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(demux->gain_q10); i++) {
		if (demux->gain_q10[i] > 0x7fff)
			return -ERANGE;
	}
	return 0;
}

static int vfe_demux_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_qcom_camss_demux *req = ctrl->p_new.p;
	unsigned long irq_flags;

	mutex_lock(&line->ife_config_lock);
	spin_lock_irqsave(&vfe->output_lock, irq_flags);
	line->ife_demux = *req;
	if (line->output.state == VFE_OUTPUT_ON) {
		int pattern = vfe_pix_pattern(line->fmt[MSM_VFE_PAD_SINK].code);

		if (pattern >= 0) {
			vfe_ife_write_demux(line, req, pattern);
			vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
		}
	}
	spin_unlock_irqrestore(&vfe->output_lock, irq_flags);
	mutex_unlock(&line->ife_config_lock);

	return 0;
}

static const struct v4l2_ctrl_ops vfe_demux_ctrl_ops = {
	.try_ctrl = vfe_demux_try_ctrl,
	.s_ctrl = vfe_demux_s_ctrl,
};

static int vfe_bpc_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_bpc *bpc = ctrl->p_new.p;

	if (bpc->enable & ~0x7U)
		return -EINVAL;
	if (!bpc->enable)
		return 0;

	if (bpc->sat_value > 0xfff ||
	    bpc->fmax < 64 || bpc->fmax > 127 ||
	    bpc->fmin > 64 ||
	    bpc->bcc_offset > 0x1fff ||
	    bpc->offset_hot > 0x3fff ||
	    bpc->offset_cold > 0x3fff)
		return -ERANGE;

	return 0;
}

static int vfe_bpc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_qcom_camss_bpc *req = ctrl->p_new.p;
	unsigned long irq_flags;

	mutex_lock(&line->ife_config_lock);
	spin_lock_irqsave(&vfe->output_lock, irq_flags);
	line->ife_bpc = *req;
	if (line->output.state == VFE_OUTPUT_ON) {
		if (req->enable) {
			vfe_ife_write_bpc(line, req);
			line->ife_lens_en |= LENS_BPC_EN;
		} else {
			line->ife_lens_en &= ~LENS_BPC_EN;
		}
		writel_relaxed(line->ife_lens_en, vfe->base + VFE_MODULE_LENS_EN);
		if (line->dual_ife && line->slave_vfe)
			writel_relaxed(line->ife_lens_en,
				       line->slave_vfe->base + VFE_MODULE_LENS_EN);
		vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
	}
	spin_unlock_irqrestore(&vfe->output_lock, irq_flags);
	mutex_unlock(&line->ife_config_lock);

	return 0;
}

static const struct v4l2_ctrl_ops vfe_bpc_ctrl_ops = {
	.try_ctrl = vfe_bpc_try_ctrl,
	.s_ctrl = vfe_bpc_s_ctrl,
};

static int vfe_abf_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_abf *abf = ctrl->p_new.p;
	unsigned int i;

	if (abf->enable & ~0x7U)
		return -EINVAL;
	if (!abf->enable)
		return 0;

	if (abf->act_th_r > 7 || abf->act_th_g > 3 || abf->act_th_b > 3 ||
	    abf->blk_pix_r > 7 || abf->blk_pix_g > 3 || abf->blk_pix_b > 3 ||
	    abf->bpc_max_shft > 7 || abf->bpc_min_shft > 7 ||
	    abf->bpc_offset > 0xfff ||
	    abf->edge_softness[0] > 63 || abf->edge_softness[1] > 63 ||
	    abf->edge_softness[2] > 15 || abf->edge_softness[3] > 15 ||
	    abf->noise_preserve_lo > 0x3ff || abf->noise_preserve_span > 0x3ff ||
	    abf->noise_preserve_base[0] > 256 || abf->noise_preserve_base[1] > 256)
		return -ERANGE;

	for (i = 0; i < ARRAY_SIZE(abf->spatial_wt); i++)
		if (abf->spatial_wt[i] > 64)
			return -ERANGE;
	for (i = 0; i < ARRAY_SIZE(abf->noise_std_lut); i++)
		if (abf->noise_std_lut[i] < 1 || abf->noise_std_lut[i] > 511)
			return -ERANGE;

	return 0;
}

static int vfe_abf_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_qcom_camss_abf *req = ctrl->p_new.p;
	unsigned long irq_flags;

	mutex_lock(&line->ife_config_lock);
	spin_lock_irqsave(&vfe->output_lock, irq_flags);
	line->ife_abf = *req;
	if (line->output.state == VFE_OUTPUT_ON) {
		if (req->enable) {
			vfe_ife_write_abf(line, req, false);
			line->ife_lens_en |= LENS_ABF_EN;
		} else {
			line->ife_lens_en &= ~LENS_ABF_EN;
		}
		writel_relaxed(line->ife_lens_en, vfe->base + VFE_MODULE_LENS_EN);
		if (line->dual_ife && line->slave_vfe)
			writel_relaxed(line->ife_lens_en,
				       line->slave_vfe->base + VFE_MODULE_LENS_EN);
		vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
	}
	spin_unlock_irqrestore(&vfe->output_lock, irq_flags);
	mutex_unlock(&line->ife_config_lock);

	return 0;
}

static const struct v4l2_ctrl_ops vfe_abf_ctrl_ops = {
	.try_ctrl = vfe_abf_try_ctrl,
	.s_ctrl = vfe_abf_s_ctrl,
};

static int vfe_gtm_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_qcom_camss_gtm *gtm = ctrl->p_new.p;
	unsigned int i;

	if (gtm->enable > 1)
		return -EINVAL;
	if (!gtm->enable)
		return 0;

	for (i = 0; i < ARRAY_SIZE(gtm->y_ratio_q12); i++)
		if (gtm->y_ratio_q12[i] > 0x3ffffU)
			return -ERANGE;

	return 0;
}

static int vfe_gtm_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vfe_line *line = ctrl->priv;
	struct vfe_device *vfe = to_vfe(line);
	const struct v4l2_qcom_camss_gtm *req = ctrl->p_new.p;
	unsigned long irq_flags;

	mutex_lock(&line->ife_config_lock);
	spin_lock_irqsave(&vfe->output_lock, irq_flags);
	line->ife_gtm = *req;
	if (line->output.state == VFE_OUTPUT_ON) {
		if (req->enable) {
			vfe_ife_write_gtm(line, req, false);
			line->ife_color_en |= COLOR_GTM_EN;
		} else {
			line->ife_color_en &= ~COLOR_GTM_EN;
		}
		writel_relaxed(line->ife_color_en, vfe->base + VFE_MODULE_COLOR_EN);
		if (line->dual_ife && line->slave_vfe)
			writel_relaxed(line->ife_color_en,
				       line->slave_vfe->base + VFE_MODULE_COLOR_EN);
		vfe->res->hw_ops->reg_update(vfe, VFE_LINE_PIX);
	}
	spin_unlock_irqrestore(&vfe->output_lock, irq_flags);
	mutex_unlock(&line->ife_config_lock);

	return 0;
}

static const struct v4l2_ctrl_ops vfe_gtm_ctrl_ops = {
	.try_ctrl = vfe_gtm_try_ctrl,
	.s_ctrl = vfe_gtm_s_ctrl,
};

int vfe_ife_ctrl_init(struct vfe_line *line)
{
	static const struct v4l2_ctrl_config config_ctrl = {
		.ops = &vfe_ife_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_IFE_CONFIG,
		.name = "IFE tuning configuration",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_ife_config) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config crop_ctrl = {
		.ops = &vfe_ife_crop_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_IFE_CROP,
		.name = "IFE request crop",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_ife_crop) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config bf_roi_ctrl = {
		.ops = &vfe_bf_roi_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_BF_ROI,
		.name = "IFE Bayer focus ROI",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_bf_roi) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config lsc_ctrl = {
		.ops = &vfe_lsc_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_LSC,
		.name = "IFE lens shading mesh",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_lsc) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config demux_ctrl = {
		.ops = &vfe_demux_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_DEMUX,
		.name = "IFE Demux channel gains",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_demux) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config bpc_ctrl = {
		.ops = &vfe_bpc_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_BPC,
		.name = "IFE bad pixel correction",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_bpc) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config abf_ctrl = {
		.ops = &vfe_abf_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_ABF,
		.name = "IFE adaptive Bayer filter",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_abf) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	static const struct v4l2_ctrl_config gtm_ctrl = {
		.ops = &vfe_gtm_ctrl_ops,
		.id = V4L2_CID_QCOM_CAMSS_GTM,
		.name = "IFE global tone mapping",
		.type = V4L2_CTRL_TYPE_U8,
		.min = 0,
		.max = U8_MAX,
		.step = 1,
		.dims = { sizeof(struct v4l2_qcom_camss_gtm) },
		.elem_size = sizeof(u8),
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	};
	struct v4l2_qcom_camss_ife_config *tuning = &line->ife_config;
	struct v4l2_ctrl *control, *demux_control;
	int ret;

	INIT_LIST_HEAD(&line->sensor_pending);
	memset(tuning, 0, sizeof(*tuning));
	memset(&line->bf_roi, 0, sizeof(line->bf_roi));
	memset(&line->ife_lsc, 0, sizeof(line->ife_lsc));
	memset(&line->ife_bpc, 0, sizeof(line->ife_bpc));
	memset(&line->ife_abf, 0, sizeof(line->ife_abf));
	memset(&line->ife_gtm, 0, sizeof(line->ife_gtm));
	line->ife_demux.gain_q10[0] = 1024;
	line->ife_demux.gain_q10[1] = 1024;
	line->ife_demux.gain_q10[2] = 1024;
	line->ife_demux.gain_q10[3] = 1024;
	tuning->version = V4L2_QCOM_CAMSS_IFE_CONFIG_VERSION;
	tuning->flags = V4L2_QCOM_CAMSS_IFE_CONFIG_ALL;
	tuning->black_level = VFE_IFE_DEFAULT_BLACK_LEVEL;
	memcpy(tuning->black_level_comp, vfe_ife_default_black_level_comp,
	       sizeof(tuning->black_level_comp));
	tuning->wb_gain_q7[0] = 128;
	tuning->wb_gain_q7[1] = 128;
	tuning->wb_gain_q7[2] = 128;
	tuning->wb_gain_q7[3] = 128;
	memcpy(line->ife_wb_gain_q7, tuning->wb_gain_q7,
	       sizeof(line->ife_wb_gain_q7));
	memcpy(tuning->ccm_q7, vfe_ife_default_ccm_q7,
	       sizeof(tuning->ccm_q7));
	memcpy(tuning->gamma, vfe_ife_default_gamma, sizeof(tuning->gamma));

	ret = v4l2_ctrl_handler_init(&line->ife_ctrls, 8);
	if (ret)
		return ret;
	control = v4l2_ctrl_new_custom(&line->ife_ctrls, &config_ctrl, line);
	v4l2_ctrl_new_custom(&line->ife_ctrls, &crop_ctrl, line);
	v4l2_ctrl_new_custom(&line->ife_ctrls, &bf_roi_ctrl, line);
	v4l2_ctrl_new_custom(&line->ife_ctrls, &lsc_ctrl, line);
	demux_control = v4l2_ctrl_new_custom(&line->ife_ctrls, &demux_ctrl, line);
	v4l2_ctrl_new_custom(&line->ife_ctrls, &bpc_ctrl, line);
	v4l2_ctrl_new_custom(&line->ife_ctrls, &abf_ctrl, line);
	v4l2_ctrl_new_custom(&line->ife_ctrls, &gtm_ctrl, line);
	ret = line->ife_ctrls.error;
	if (!ret)
		ret = v4l2_ctrl_s_ctrl_compound(control, V4L2_CTRL_TYPE_U8, tuning);
	if (!ret)
		ret = v4l2_ctrl_s_ctrl_compound(demux_control, V4L2_CTRL_TYPE_U8,
						&line->ife_demux);
	if (ret) {
		v4l2_ctrl_handler_free(&line->ife_ctrls);
		return ret;
	}

	line->subdev.ctrl_handler = &line->ife_ctrls;
	line->ife_ctrls_initialized = true;

	return 0;
}

void vfe_ife_ctrl_cleanup(struct vfe_line *line)
{
	if (!line->ife_ctrls_initialized)
		return;

	line->subdev.ctrl_handler = NULL;
	v4l2_ctrl_handler_free(&line->ife_ctrls);
	line->ife_ctrls_initialized = false;
}

static void vfe_pix_program_common(struct vfe_device *vfe,
				   const struct vfe_line *line,
				   int pattern, u32 even_pat, u32 odd_pat,
				   u32 in_width, u32 in_height)
{
	u32 val;

	writel_relaxed(pattern, vfe->base + VFE_CORE_CFG);
	writel_relaxed(in_width - 1, vfe->base + VFE_CAMIF_RAW_CROP_WIDTH_CFG);
	writel_relaxed(in_height - 1, vfe->base + VFE_CAMIF_RAW_CROP_HEIGHT_CFG);
	writel_relaxed(CMD_CLEAR_CAMIF_STATUS, vfe->base + VFE_CAMIF_CMD);

	writel_relaxed(1, vfe->base + VFE_DEMUX_CFG);
	writel_relaxed(even_pat, vfe->base + VFE_DEMUX_CFG + 20);
	writel_relaxed(odd_pat, vfe->base + VFE_DEMUX_CFG + 24);

	writel_relaxed(0x100, vfe->base + VFE_DEMO_CFG);
	writel_relaxed(0x00008000, vfe->base + VFE_DEMO_INTERP_COEFF_CFG);
	writel_relaxed(0x08000066, vfe->base + VFE_DEMO_INTERP_CLASSIFIER_0);

	writel_relaxed(FULL_OUT_ROUND_SHIFT(2) | 6,
		       vfe->base + VFE_FULL_OUT_Y_ROUND_CFG);
	writel_relaxed(FULL_OUT_ROUND_SHIFT(2) | 7,
		       vfe->base + VFE_FULL_OUT_C_ROUND_CFG);
	val = FULL_OUT_CROP_EN | FULL_OUT_ROUND_EN | FULL_OUT_CLAMP_EN;
	writel_relaxed(val, vfe->base + VFE_FULL_OUT_Y_CROP_RND_CLAMP_CFG);
	writel_relaxed(val, vfe->base + VFE_FULL_OUT_C_CROP_RND_CLAMP_CFG);

	writel_relaxed(line->ife_lens_en, vfe->base + VFE_MODULE_LENS_EN);
	writel_relaxed(line->ife_color_en, vfe->base + VFE_MODULE_COLOR_EN);
	val = ZOOM_CST_EN | ZOOM_SCALE_VID_EN | ZOOM_CROP_VID_EN;
	writel_relaxed(val, vfe->base + VFE_MODULE_ZOOM_EN);

	writel_relaxed(CFG_VFE_OUTPUT_EN | CFG_BUS_OUTPUT_EN | CFG_RAW_CROP_EN,
		       vfe->base + VFE_CAMIF_CFG);
	writel_relaxed(0xffff, vfe->base + VFE_CAMIF_LINE_SKIP_PATTERN);
	writel_relaxed(0xffff, vfe->base + VFE_CAMIF_PIXEL_SKIP_PATTERN);
	writel_relaxed(0, vfe->base + VFE_CAMIF_SKIP_PERIOD);
	writel_relaxed(~0U, vfe->base + VFE_CAMIF_IRQ_SUBSAMPLE_PATTERN);
	writel_relaxed(BIT(3) | BIT(4), vfe->base + VFE_BUS_COMP_GRP_0);
}

static int vfe_pix_program(struct vfe_line *line)
{
	static const struct {
		u32 even;
		u32 odd;
	} demux_patterns[] = {
		{ 0xc9, 0xac }, /* RGGB */
		{ 0x9c, 0xca }, /* GRBG */
		{ 0xca, 0x9c }, /* BGGR */
		{ 0xac, 0xc9 }, /* GBRG */
	};
	struct vfe_device *vfe = to_vfe(line);
	struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	struct v4l2_qcom_camss_ife_crop crop = vfe_pix_default_crop(line);
	u32 val;
	int pattern;

	pattern = vfe_pix_pattern(sink->code);
	if (pattern < 0) {
		dev_err(vfe->camss->dev, "Unsupported Bayer format 0x%x\n",
			sink->code);
		return -EINVAL;
	}

	/* Demux 13 uses Q10 gains and Bayer even/odd line patterns. */
	mutex_lock(&line->ife_config_lock);
	vfe_ife_write_demux(line, &line->ife_demux, pattern);

	/* Program factory tuning before enabling the IFE color modules. */
	vfe_ife_write_config(line, &line->ife_config,
			     V4L2_QCOM_CAMSS_IFE_CONFIG_ALL, true);

	val = LENS_DEMUX_EN | LENS_DEMOSAIC_EN | LENS_BLS_EN;
	if (line->ife_bpc.enable) {
		vfe_ife_write_bpc(line, &line->ife_bpc);
		val |= LENS_BPC_EN;
	}
	if (line->ife_abf.enable) {
		vfe_ife_write_abf(line, &line->ife_abf, true);
		val |= LENS_ABF_EN;
	}
	if (line->ife_lsc.enable) {
		int ret = vfe_ife_write_lsc(line, &line->ife_lsc, true);

		if (ret) {
			mutex_unlock(&line->ife_config_lock);
			return ret;
		}
		val |= LENS_ROLLOFF_EN;
	}
	line->ife_lens_en = val;

	line->ife_color_en = COLOR_CC_EN | COLOR_GAMMA_EN;
	if (line->ife_gtm.enable) {
		vfe_ife_write_gtm(line, &line->ife_gtm, true);
		line->ife_color_en |= COLOR_GTM_EN;
	}

	vfe_pix_write_cst(line);
	vfe_pix_write_crop(line, &crop);

	vfe_pix_program_common(vfe, line, pattern,
			       demux_patterns[pattern].even,
			       demux_patterns[pattern].odd,
			       line->dual_ife ? line->dual_left_in_w :
						sink->width,
			       sink->height);
	if (line->dual_ife && line->slave_vfe)
		vfe_pix_program_common(line->slave_vfe, line, pattern,
				       demux_patterns[pattern].even,
				       demux_patterns[pattern].odd,
				       line->dual_right_in_w,
				       sink->height);
	mutex_unlock(&line->ife_config_lock);

	return vfe_17x_stats_start(line);
}

static void vfe_global_reset(struct vfe_device *vfe)
{
	u32 reset_bits = GLOBAL_RESET_CMD_CORE		|
			 GLOBAL_RESET_CMD_CAMIF		|
			 GLOBAL_RESET_CMD_BUS		|
			 GLOBAL_RESET_CMD_BUS_BDG	|
			 GLOBAL_RESET_CMD_REGISTER	|
			 GLOBAL_RESET_CMD_TESTGEN	|
			 GLOBAL_RESET_CMD_DSP		|
			 GLOBAL_RESET_CMD_IDLE_CGC	|
			 GLOBAL_RESET_CMD_RDI0		|
			 GLOBAL_RESET_CMD_RDI1		|
			 GLOBAL_RESET_CMD_RDI2		|
			 GLOBAL_RESET_CMD_RDI3;

	writel_relaxed(BIT(31), vfe->base + VFE_IRQ_MASK_0);

	/* Make sure IRQ mask has been written before resetting */
	wmb();

	writel_relaxed(reset_bits, vfe->base + VFE_GLOBAL_RESET_CMD);
}

static void vfe_wm_done_irq(struct vfe_device *vfe, u8 wm, bool enable)
{
	struct vfe_device *lock_vfe = vfe->dual_master_vfe ?: vfe;
	bool composite = !vfe->res->is_lite && wm >= 3;
	unsigned int bank = composite ? 0 : 1;
	u32 bit = composite ? STATUS0_COMP0_BUF_DONE : STATUS1_WM_CLIENT_BUF_DONE(wm);
	u32 mask;

	/* The two PIX planes share one completion interrupt. */
	if (composite && wm != 3)
		return;

	lockdep_assert_held(&lock_vfe->output_lock);
	mask = readl_relaxed(vfe->base + VFE_BUS_IRQ_MASK(bank));
	if (enable) {
		writel_relaxed(bit, vfe->base + VFE_BUS_IRQ_CLEAR(bank));
		writel(1, vfe->base + VFE_BUS_IRQ_CLEAR_GLOBAL);
		mask |= bit;
	} else {
		mask &= ~bit;
	}
	writel(mask, vfe->base + VFE_BUS_IRQ_MASK(bank));
}

static void vfe_wm_start(struct vfe_device *vfe, u8 wm, struct vfe_line *line)
{
	u32 val;

	if (!vfe->res->is_lite && wm == 3 && vfe != line->slave_vfe) {
		line->pix_done_pending = false;
		line->slave_pix_done_pending = false;
	}
	vfe_wm_done_irq(vfe, wm, true);

	writel_relaxed(0xf, vfe->base + VFE_BUS_WM_BURST_LIMIT(wm));

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		u32 width, height, stride, frame_inc;

		vfe_pix_wm_geometry(line, vfe, wm, &width, &height, &stride,
				    &frame_inc);

		writel_relaxed(width,
			       vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));
		writel_relaxed(height,
			       vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));
		writel_relaxed(0xe, vfe->base + VFE_BUS_WM_PACKER_CFG(wm));
		writel_relaxed(stride, vfe->base + VFE_BUS_WM_STRIDE(wm));
		writel_relaxed(frame_inc, vfe->base + VFE_BUS_WM_FRAME_INC(wm));

		val = 1 << WM_CFG_EN | MODE_QCOM_PLAIN << WM_CFG_MODE;
		writel_relaxed(val, vfe->base + VFE_BUS_WM_CFG(wm));
		if (line->dual_ife && line->slave_vfe && vfe != line->slave_vfe)
			vfe_wm_start(line->slave_vfe, wm, line);
		return;
	}

	val = WM_BUFFER_DEFAULT_WIDTH;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));

	val = 0;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));

	val = 0;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_PACKER_CFG(wm));

	/* Configure stride for RDIs */
	val = WM_STRIDE_DEFAULT_STRIDE;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_STRIDE(wm));

	/* Enable WM */
	val = 1 << WM_CFG_EN |
	      MODE_MIPI_RAW << WM_CFG_MODE;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_CFG(wm));
}

static void vfe_wm_stop(struct vfe_device *vfe, u8 wm)
{
	vfe_wm_done_irq(vfe, wm, false);
	writel(0, vfe->base + VFE_BUS_WM_CFG(wm));
	readl(vfe->base + VFE_BUS_WM_CFG(wm));
	if (!vfe->res->is_lite && wm >= 3 &&
	    vfe->line[VFE_LINE_PIX].dual_ife &&
	    vfe->line[VFE_LINE_PIX].slave_vfe &&
	    vfe != vfe->line[VFE_LINE_PIX].slave_vfe)
		vfe_wm_stop(vfe->line[VFE_LINE_PIX].slave_vfe, wm);
}

static void vfe_wm_update(struct vfe_device *vfe, u8 wm, u32 addr,
			  struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;
	u32 stride = pix->plane_fmt[0].bytesperline;
	u32 width, height, plane_stride, frame_inc;

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		vfe_pix_wm_geometry(line, vfe, wm, &width, &height,
				    &plane_stride, &frame_inc);
		writel_relaxed(width,
			       vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));
		writel_relaxed(height,
			       vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));
		writel_relaxed(plane_stride, vfe->base + VFE_BUS_WM_STRIDE(wm));
	}

	writel_relaxed(addr, vfe->base + VFE_BUS_WM_IMAGE_ADDR(wm));
	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		writel_relaxed(frame_inc, vfe->base + VFE_BUS_WM_FRAME_INC(wm));
		writel_relaxed(1 << WM_CFG_EN | MODE_QCOM_PLAIN << WM_CFG_MODE,
			       vfe->base + VFE_BUS_WM_CFG(wm));
		if (line->dual_ife && line->slave_vfe && vfe != line->slave_vfe)
			vfe_wm_update(line->slave_vfe, wm, addr + width, line);
	} else {
		writel_relaxed(stride * pix->height,
			       vfe->base + VFE_BUS_WM_FRAME_INC(wm));
	}
}

static u32 vfe_reg_update_line(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	if (line_id == VFE_LINE_PIX && !vfe->res->is_lite)
		return REG_UPDATE_PIX;

	return REG_UPDATE_RDI(line_id);
}

static void vfe_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	vfe->reg_update |= vfe_reg_update_line(vfe, line_id);

	/* Enforce ordering between previous reg writes and reg update */
	wmb();

	writel_relaxed(vfe->reg_update, vfe->base + VFE_REG_UPDATE_CMD);

	/* Enforce ordering between reg update and subsequent reg writes */
	wmb();

	if (line_id == VFE_LINE_PIX && !vfe->res->is_lite &&
	    vfe->line[VFE_LINE_PIX].dual_ife &&
	    vfe->line[VFE_LINE_PIX].slave_vfe &&
	    vfe != vfe->line[VFE_LINE_PIX].slave_vfe)
		vfe_reg_update(vfe->line[VFE_LINE_PIX].slave_vfe, line_id);
}

static inline void vfe_reg_update_clear(struct vfe_device *vfe,
					enum vfe_line_id line_id)
{
	vfe->reg_update &= ~vfe_reg_update_line(vfe, line_id);
	if (line_id == VFE_LINE_PIX && !vfe->res->is_lite &&
	    vfe->line[VFE_LINE_PIX].dual_ife &&
	    vfe->line[VFE_LINE_PIX].slave_vfe &&
	    vfe != vfe->line[VFE_LINE_PIX].slave_vfe &&
	    vfe->line[VFE_LINE_PIX].output.state != VFE_OUTPUT_ON)
		vfe->line[VFE_LINE_PIX].slave_vfe->reg_update &=
			~vfe_reg_update_line(vfe->line[VFE_LINE_PIX].slave_vfe,
					     line_id);
}

static void vfe_init_bus(struct vfe_device *vfe)
{
	u32 val;

	val = DEBUG_STATUS_CFG_STATUS0(1) |
	      DEBUG_STATUS_CFG_STATUS0(7);
	writel_relaxed(val, vfe->base + VFE_BUS_WM_DEBUG_STATUS_CFG);

	/* BUS_WM_INPUT_IF_ADDR_SYNC_FRAME_HEADER */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_ADDR_SYNC_FRAME_HEADER);

	/* no clock gating at bus input */
	val = WM_CGC_OVERRIDE_ALL;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_CGC_OVERRIDE);

	writel_relaxed(0x0, vfe->base + VFE_BUS_WM_TEST_BUS_CTRL);

	val = WM_ADDR_NO_SYNC_DEFAULT_VAL;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_ADDR_SYNC_NO_SYNC);
}

static void vfe_enable_irq_common(struct vfe_device *vfe)
{
	u32 mask = STATUS_0_RESET_ACK;
	unsigned int i;

	/* Bus completions can occur independently of the frame boundary. */
	if (vfe->camss->res->version == CAMSS_8150)
		mask |= STATUS_0_BUS_WR_IRQ;

	for (i = 0; i < (vfe->res->is_lite ? 4 : 3); i++)
		mask |= STATUS_0_RDI_REG_UPDATE(i) | STATUS_1_RDI_SOF(i);
	if (!vfe->res->is_lite)
		mask |= STATUS_0_CAMIF_SOF | STATUS_0_PIX_REG_UPDATE;
	writel_relaxed(mask, vfe->base + VFE_IRQ_MASK_0);
	writel_relaxed(STATUS_1_VIOLATION, vfe->base + VFE_IRQ_MASK_1);
	writel_relaxed(VFE_BUS_ERROR_MASK, vfe->base + VFE_BUS_IRQ_MASK(0));
	writel_relaxed(vfe->res->is_lite ? 0 : VFE_17X_STATS_IRQ_MASK,
		       vfe->base + VFE_BUS_IRQ_MASK(1));
	writel_relaxed(0, vfe->base + VFE_BUS_IRQ_MASK(2));
}

static void vfe_isr_halt_ack(struct vfe_device *vfe)
{
	complete(&vfe->halt_complete);
}

static void vfe_isr_read(struct vfe_device *vfe, u32 *status0, u32 *status1)
{
	*status0 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_0);
	*status1 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_1);

	writel_relaxed(*status0, vfe->base + VFE_IRQ_CLEAR_0);
	writel_relaxed(*status1, vfe->base + VFE_IRQ_CLEAR_1);

	/* Enforce ordering between IRQ Clear and Global IRQ Clear */
	wmb();
	writel_relaxed(CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);
}

static void vfe_violation_read(struct vfe_device *vfe)
{
	u32 violation = readl_relaxed(vfe->base + VFE_VIOLATION_STATUS);

	pr_err_ratelimited("VFE: violation = 0x%08x\n", violation);
}

static void vfe_pix_complete(struct vfe_device *vfe, bool completed)
{
	struct vfe_line *line = &vfe->line[VFE_LINE_PIX];
	struct vfe_output *output = &line->output;
	struct camss_buffer *buffer;
	bool ready;

	spin_lock(&vfe->output_lock);
	line->pix_done_pending |= completed;
	buffer = output->buf[0];
	ready = line->pix_done_pending &&
		(!line->dual_ife || line->slave_pix_done_pending) &&
		output->state == VFE_OUTPUT_ON;

	/* Request timing needs the SOF following the captured frame. */
	if (ready && line->video_out.vb2_q.uses_requests && buffer &&
	    (s32)(output->sof_sequence - (buffer->input_sequence + 2)) < 0)
		ready = false;
	if (ready) {
		line->pix_done_pending = false;
		line->slave_pix_done_pending = false;
	}
	spin_unlock(&vfe->output_lock);

	if (ready)
		vfe->isr_ops.wm_done(vfe, 3);
}

static void vfe_dual_slave_comp_done(struct vfe_device *master)
{
	struct vfe_line *line = &master->line[VFE_LINE_PIX];

	spin_lock(&master->output_lock);
	line->slave_pix_done_pending = true;
	spin_unlock(&master->output_lock);
	vfe_pix_complete(master, false);
}

static void vfe_schedule_recovery(struct vfe_device *vfe)
{
	unsigned int i;

	lockdep_assert_held(&vfe->output_lock);
	if (vfe->camss->res->version != CAMSS_8150 || vfe->recovery_disabled ||
	    vfe->fault_pending)
		return;

	for (i = 0; i < vfe->res->line_num; i++) {
		if (vfe->line[i].output.state == VFE_OUTPUT_ON) {
			vfe->fault_pending = true;
			/* Keep reset acknowledgment enabled while discarding frame IRQs. */
			writel_relaxed(STATUS_0_RESET_ACK, vfe->base + VFE_IRQ_MASK_0);
			writel(0, vfe->base + VFE_IRQ_MASK_1);
			schedule_work(&vfe->recover_work);
			break;
		}
	}
}

/*
 * vfe_isr - VFE module interrupt handler
 * @irq: Interrupt line
 * @dev: VFE device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t vfe_isr(int irq, void *dev)
{
	struct vfe_device *vfe = dev;
	u32 status0, status1, vfe_bus_status[3];
	bool fault_pending;
	int i, wm;

	status0 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_0);
	status1 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_1);

	writel_relaxed(status0, vfe->base + VFE_IRQ_CLEAR_0);
	writel_relaxed(status1, vfe->base + VFE_IRQ_CLEAR_1);

	spin_lock(&vfe->output_lock);
	for (i = 0; i < ARRAY_SIZE(vfe_bus_status); i++) {
		vfe_bus_status[i] = readl_relaxed(vfe->base + VFE_BUS_IRQ_STATUS(i));
		writel_relaxed(vfe_bus_status[i], vfe->base + VFE_BUS_IRQ_CLEAR(i));
		vfe_bus_status[i] &= readl_relaxed(vfe->base + VFE_BUS_IRQ_MASK(i));
	}

	/* Enforce ordering between IRQ reading and interpretation */
	wmb();

	writel_relaxed(CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);
	writel_relaxed(1, vfe->base + VFE_BUS_IRQ_CLEAR_GLOBAL);
	if ((status1 & STATUS_1_VIOLATION) || (vfe_bus_status[0] & VFE_BUS_ERROR_MASK))
		vfe_schedule_recovery(vfe);
	fault_pending = vfe->fault_pending;
	spin_unlock(&vfe->output_lock);

	if (status1 & STATUS_1_VIOLATION)
		vfe_violation_read(vfe);
	if (vfe_bus_status[0] & VFE_BUS_ERROR_MASK)
		dev_err_ratelimited(vfe->camss->dev,
				    "VFE%u bus error %08x comp=%08x overwrite=%08x dbg=%08x\n",
				    vfe->id, vfe_bus_status[0],
				    readl_relaxed(vfe->base + VFE_BUS_COMP_ERROR_STATUS),
				    readl_relaxed(vfe->base + VFE_BUS_COMP_OVERWRITE_STATUS),
				    readl_relaxed(vfe->base + VFE_BUS_DEBUG_STATUS_0));

	if (status0 & STATUS_0_RESET_ACK)
		vfe->isr_ops.reset_ack(vfe);
	if (vfe->dual_master_vfe) {
		struct vfe_device *master = vfe->dual_master_vfe;

		if (status0 & STATUS_0_PIX_REG_UPDATE)
			vfe->isr_ops.reg_update(vfe, VFE_LINE_PIX);
		if ((status1 & STATUS_1_VIOLATION) ||
		    (vfe_bus_status[0] & VFE_BUS_ERROR_MASK)) {
			spin_lock(&master->output_lock);
			vfe_schedule_recovery(master);
			spin_unlock(&master->output_lock);
			return IRQ_HANDLED;
		}
		if (vfe_bus_status[0] & STATUS0_COMP0_BUF_DONE)
			vfe_dual_slave_comp_done(master);
		return IRQ_HANDLED;
	}
	if (fault_pending)
		return IRQ_HANDLED;

	for (i = VFE_LINE_RDI0; i < (vfe->res->is_lite ? 4 : 3); i++)
		if (status0 & STATUS_0_RDI_REG_UPDATE(i))
			vfe->isr_ops.reg_update(vfe, i);

	if (!vfe->res->is_lite && vfe->res->line_num > VFE_LINE_PIX) {
		if (status0 & STATUS_0_PIX_REG_UPDATE)
			vfe->isr_ops.reg_update(vfe, VFE_LINE_PIX);
		if (status0 & STATUS_0_CAMIF_SOF)
			vfe->isr_ops.sof(vfe, VFE_LINE_PIX);
	}

	for (i = VFE_LINE_RDI0; i < (vfe->res->is_lite ? 4 : 3); i++)
		if (status0 & STATUS_1_RDI_SOF(i))
			vfe->isr_ops.sof(vfe, i);

	for (wm = 0; wm < (vfe->res->is_lite ? 4 : 3); wm++)
		if (vfe_bus_status[1] & STATUS1_WM_CLIENT_BUF_DONE(wm))
			vfe->isr_ops.wm_done(vfe, wm);

	if (!vfe->res->is_lite)
		vfe_17x_stats_isr(vfe, vfe_bus_status[1]);

	if (!vfe->res->is_lite && vfe->res->line_num > VFE_LINE_PIX &&
	    ((status0 & STATUS_0_CAMIF_SOF) || (vfe_bus_status[0] & STATUS0_COMP0_BUF_DONE)))
		vfe_pix_complete(vfe, vfe_bus_status[0] & STATUS0_COMP0_BUF_DONE);

	return IRQ_HANDLED;
}

/*
 * vfe_halt - Trigger halt on VFE module and wait to complete
 * @vfe: VFE device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_halt(struct vfe_device *vfe)
{
	/* rely on vfe_disable_output() to stop the VFE */
	return 0;
}

static int vfe_get_output(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (output->state > VFE_OUTPUT_RESERVED) {
		dev_err(vfe->camss->dev, "Output is running\n");
		ret = -EINVAL;
		goto unlock;
	}

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		output->wm_num = 2;
		output->wm_idx[0] = 3;
		output->wm_idx[1] = 4;
	} else {
		output->wm_num = 1;
		output->wm_idx[0] = line->id;
	}

	for (i = 0; i < output->wm_num; i++) {
		if (vfe->wm_output_map[output->wm_idx[i]] != VFE_LINE_NONE) {
			dev_err(vfe->camss->dev, "Write master %u is busy\n",
				output->wm_idx[i]);
			ret = -EBUSY;
			goto unlock;
		}
	}
	for (i = 0; i < output->wm_num; i++)
		vfe->wm_output_map[output->wm_idx[i]] = line->id;
	output->drop_update_idx = 0;

unlock:
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	return ret;
}

static int vfe_dual_acquire(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct camss *camss = vfe->camss;
	struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	u8 slave_id = vfe->id ^ 1;
	struct vfe_device *slave_vfe;
	struct csid_device *master_csid, *slave_csid;
	u16 left_w, right_x, right_w;
	int ret;

	if (vfe->id > 1 || slave_id >= camss->res->vfe_num ||
	    slave_id >= camss->res->csid_num)
		return -ENODEV;

	slave_vfe = &camss->vfe[slave_id];
	master_csid = &camss->csid[vfe->id];
	slave_csid = &camss->csid[slave_id];
	if (slave_vfe->res->is_lite)
		return -ENODEV;

	mutex_lock(&slave_vfe->power_lock);
	if (slave_vfe->power_count > 0 || slave_vfe->stream_count > 0 ||
	    slave_vfe->dual_master_vfe) {
		mutex_unlock(&slave_vfe->power_lock);
		return -EBUSY;
	}
	slave_vfe->dual_master_vfe = vfe;
	mutex_unlock(&slave_vfe->power_lock);

	/*
	 * Each VFE line buffer supports up to 4096 pixels; a 3584-pixel
	 * stripe width (56 * 64) gives a 2512-pixel central overlap on
	 * 4656-wide frames while keeping AWB_BG regions 64-aligned.
	 */
	left_w = min_t(u32, 3584, sink->width);
	right_x = (sink->width - left_w) & ~3U;
	right_w = sink->width - right_x;

	ret = msm_csid_dual_setup(master_csid, slave_csid,
				  left_w - 1, right_x, sink->width - 1);
	if (ret) {
		slave_vfe->dual_master_vfe = NULL;
		return ret;
	}

	mutex_lock(&slave_vfe->stream_lock);
	vfe_init_bus(slave_vfe);
	vfe_enable_irq_common(slave_vfe);
	slave_vfe->stream_count = 1;
	slave_vfe->wm_output_map[3] = VFE_LINE_PIX;
	slave_vfe->wm_output_map[4] = VFE_LINE_PIX;
	mutex_unlock(&slave_vfe->stream_lock);

	line->dual_left_in_w = left_w;
	line->dual_right_in_x = right_x;
	line->dual_right_in_w = right_w;
	line->slave_vfe = slave_vfe;
	line->master_csid = master_csid;
	line->slave_pix_done_pending = false;
	line->dual_ife = true;

	return 0;
}

static void vfe_dual_release(struct vfe_line *line)
{
	struct vfe_device *slave_vfe = line->slave_vfe;
	struct csid_device *master_csid = line->master_csid;
	unsigned long flags;

	if (!line->dual_ife)
		return;

	line->dual_ife = false;
	line->slave_pix_done_pending = false;
	line->slave_vfe = NULL;
	line->master_csid = NULL;

	if (slave_vfe) {
		mutex_lock(&slave_vfe->stream_lock);
		vfe_release_wm(slave_vfe, 3);
		vfe_release_wm(slave_vfe, 4);
		slave_vfe->stream_count = 0;
		vfe_reset(slave_vfe);
		spin_lock_irqsave(&slave_vfe->output_lock, flags);
		slave_vfe->reg_update = 0;
		slave_vfe->fault_pending = false;
		slave_vfe->recovery_attempted = false;
		spin_unlock_irqrestore(&slave_vfe->output_lock, flags);
		mutex_unlock(&slave_vfe->stream_lock);
	}

	msm_csid_dual_cleanup(master_csid);
	if (slave_vfe)
		slave_vfe->dual_master_vfe = NULL;
}

static int vfe_disable_17x(struct vfe_line *line)
{
	int ret = vfe_disable(line);

	if (line->id == VFE_LINE_PIX && line->dual_ife)
		vfe_dual_release(line);

	return ret;
}

/*
 * vfe_enable - Enable streaming on VFE line
 * @line: VFE line
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_enable(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	int ret;

	mutex_lock(&vfe->stream_lock);

	if (!vfe->stream_count) {
		if (!line->video_out.recovering) {
			unsigned long flags;

			spin_lock_irqsave(&vfe->output_lock, flags);
			vfe->fault_pending = false;
			vfe->recovery_attempted = false;
			spin_unlock_irqrestore(&vfe->output_lock, flags);
		}
		vfe_init_bus(vfe);
		vfe_enable_irq_common(vfe);
	}

	vfe->stream_count++;

	mutex_unlock(&vfe->stream_lock);

	ret = vfe_get_output(line);
	if (ret < 0)
		goto error_get_output;

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		if (line->fmt[MSM_VFE_PAD_SINK].width > 4096) {
			ret = vfe_dual_acquire(line);
			if (ret < 0)
				goto error_enable_output;
		}
		ret = vfe_pix_program(line);
		if (ret < 0)
			goto error_enable_output;
	}

	ret = vfe_enable_output_v2(line);
	if (ret < 0)
		goto error_enable_output;
	if (line->video_out.vb2_q.uses_requests && line->output.buf[0] &&
	    !line->output.buf[0]->dummy) {
		unsigned long flags;

		spin_lock_irqsave(&vfe->output_lock, flags);
		vfe_ife_apply_request(line, line->output.buf[0]);
		spin_unlock_irqrestore(&vfe->output_lock, flags);
	}

	vfe->was_streaming = 1;

	return 0;

error_enable_output:
	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite && line->stats) {
		unsigned long flags;

		spin_lock_irqsave(&vfe->output_lock, flags);
		vfe_17x_stats_stop(line);
		spin_unlock_irqrestore(&vfe->output_lock, flags);
	}
	if (line->id == VFE_LINE_PIX && line->dual_ife)
		vfe_dual_release(line);
	vfe_put_output(line);

error_get_output:
	mutex_lock(&vfe->stream_lock);

	vfe->stream_count--;

	mutex_unlock(&vfe->stream_lock);

	return ret;
}

/*
 * vfe_isr_sof - Process start of frame interrupt
 * @vfe: VFE Device
 * @line_id: VFE line
 */
static void vfe_isr_sof(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	struct vfe_line *line = &vfe->line[line_id];
	struct vfe_output *output = &line->output;
	struct v4l2_event event = { .type = V4L2_EVENT_FRAME_SYNC };
	unsigned long flags;
	u64 timestamp = ktime_get_ns();
	unsigned int i;

	spin_lock_irqsave(&vfe->output_lock, flags);
	if (output->state == VFE_OUTPUT_ON) {
		event.u.frame_sync.frame_sequence = output->sof_sequence++;
		if (line->video_out.vb2_q.uses_requests) {
			for (i = 0; i < ARRAY_SIZE(output->buf); i++) {
				struct camss_buffer *buf = output->buf[i];

				if (!buf || buf->dummy)
					continue;

				if (buf->input_sequence == event.u.frame_sync.frame_sequence)
					buf->ife.sensor_timestamp = timestamp;
				else if (buf->input_sequence + 1 == event.u.frame_sync.frame_sequence &&
					 buf->ife.sensor_timestamp)
					buf->ife.frame_duration_us = div_u64(timestamp -
									buf->ife.sensor_timestamp,
									NSEC_PER_USEC);
			}
			vfe_ife_schedule_request(line);
			vfe_sensor_schedule(line);
		}
		v4l2_event_queue(line->subdev.devnode, &event);
	}
	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

/*
 * vfe_isr_reg_update - Process reg update interrupt
 * @vfe: VFE Device
 * @line_id: VFE line
 */
static void vfe_isr_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	struct vfe_output *output;
	unsigned long flags;

	spin_lock_irqsave(&vfe->output_lock, flags);
	vfe->res->hw_ops->reg_update_clear(vfe, line_id);

	output = &vfe->line[line_id].output;

	if (output->wait_reg_update) {
		output->wait_reg_update = 0;
		complete(&output->reg_update);
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

/*
 * vfe_isr_wm_done - Process write master done interrupt
 * @vfe: VFE Device
 * @wm: Write master id
 */
static void vfe_isr_wm_done(struct vfe_device *vfe, u8 wm)
{
	struct vfe_line *line;
	struct camss_buffer *ready_buf;
	struct vfe_output *output;
	unsigned long flags;
	u32 index;
	u64 ts = ktime_get_ns();
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
	unsigned int i;

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (vfe->wm_output_map[wm] == VFE_LINE_NONE) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Received wm done for unmapped index\n");
		goto out_unlock;
	}
	line = &vfe->line[vfe->wm_output_map[wm]];
	output = &line->output;
	if (output->state != VFE_OUTPUT_ON)
		goto out_unlock;

	ready_buf = output->buf[0];
	if (!ready_buf) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Missing ready buf %d!\n", output->state);
		goto out_unlock;
	}

	if (!ready_buf->dummy) {
		ready_buf->vb.vb2_buf.timestamp = ts;
		ready_buf->vb.sequence = output->sof_sequence - 1;
		if (ready_buf->ife_valid &&
		    (!ready_buf->ife_applied || !ready_buf->sensor_applied ||
		     !ready_buf->ife.sensor_timestamp ||
		     ready_buf->input_sequence + 1 != ready_buf->vb.sequence)) {
			dev_err_ratelimited(vfe->camss->dev,
					    "IFE request input %u returned at DMA frame %u, IFE %u sensor %u\n",
					    ready_buf->input_sequence, ready_buf->vb.sequence,
					    ready_buf->ife_applied, ready_buf->sensor_applied);
			state = VB2_BUF_STATE_ERROR;
		}
		if (state == VB2_BUF_STATE_DONE)
			vfe->recovery_attempted = false;
	}

	index = 0;
	output->buf[0] = output->buf[1];
	if (output->buf[0])
		index = 1;

	output->buf[index] = NULL;
	if (line->video_out.vb2_q.uses_requests)
		output->buf[index] = vfe_next_request_buffer(line, output->sof_sequence);
	else
		output->buf[index] = vfe_buf_get_pending(output);

	if (output->buf[index]) {
		for (i = 0; i < output->wm_num; i++)
			vfe_wm_update(vfe, output->wm_idx[i],
				      output->buf[index]->addr[i], line);
	} else {
		output->gen2.active_num--;
	}

	if (line->video_out.vb2_q.uses_requests)
		vfe->res->hw_ops->reg_update(vfe, line->id);
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	camss_buffer_done(ready_buf, state);

	return;

out_unlock:
	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

static const struct vfe_isr_ops vfe_isr_ops_170 = {
	.reset_ack = vfe_isr_reset_ack,
	.halt_ack = vfe_isr_halt_ack,
	.reg_update = vfe_isr_reg_update,
	.sof = vfe_isr_sof,
	.comp_done = vfe_isr_comp_done,
	.wm_done = vfe_isr_wm_done,
};

static long vfe_get_frame_sequence(struct camss_video *video, u32 *sequence)
{
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);
	struct vfe_device *vfe = to_vfe(line);
	unsigned long flags;

	if (line->id != VFE_LINE_PIX || vfe->res->is_lite)
		return -EINVAL;

	spin_lock_irqsave(&vfe->output_lock, flags);
	*sequence = line->output.state == VFE_OUTPUT_ON ?
		    line->output.sof_sequence : 0;
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;
}

static long vfe_get_request_sequence(struct camss_video *video, u32 *sequence)
{
	struct vfe_line *line = container_of(video, struct vfe_line, video_out);
	struct vfe_device *vfe = to_vfe(line);
	struct camss_buffer *buf;
	unsigned long flags;
	long ret = 0;

	if (line->id != VFE_LINE_PIX || vfe->res->is_lite)
		return -EOPNOTSUPP;

	spin_lock_irqsave(&vfe->output_lock, flags);
	if (line->output.state != VFE_OUTPUT_ON) {
		*sequence = 0;
	} else if (!video->vb2_q.uses_requests) {
		ret = -EOPNOTSUPP;
	} else {
		s32 prev_vblank = line->sensor_inflight ?
				  line->sensor_inflight->sensor_vblank :
				  line->sensor_vblank;

		*sequence = max(line->output.sof_sequence + 1, line->sensor_next + 2);
		if (line->sensor_inflight)
			(*sequence)++;
		list_for_each_entry(buf, &line->sensor_pending, queue) {
			if (buf->sensor_vblank != prev_vblank) {
				(*sequence)++;
				prev_vblank = buf->sensor_vblank;
			}
			(*sequence)++;
		}
	}
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	return ret;
}

static const struct camss_video_ops vfe_video_ops_170 = {
	.queue_buffer = vfe_queue_request_buffer,
	.flush_buffers = vfe_flush_buffers,
	.get_stats = vfe_17x_get_stats,
	.get_frame_sequence = vfe_get_frame_sequence,
	.get_request_sequence = vfe_get_request_sequence,
	.prepare_request = vfe_prepare_request,
	.start_requests = vfe_start_requests,
	.stop_requests = vfe_stop_requests,
	.apply_sensor = vfe_apply_sensor,
};

static void vfe_subdev_init(struct device *dev, struct vfe_device *vfe)
{
	vfe->isr_ops = vfe_isr_ops_170;
	vfe->video_ops = vfe_video_ops_170;
	INIT_WORK(&vfe->recover_work, vfe_recover);
}

const struct vfe_hw_ops vfe_ops_170 = {
	.global_reset = vfe_global_reset,
	.hw_version = vfe_hw_version,
	.isr_read = vfe_isr_read,
	.isr = vfe_isr,
	.pm_domain_off = vfe_pm_domain_off,
	.pm_domain_on = vfe_pm_domain_on,
	.reg_update_clear = vfe_reg_update_clear,
	.reg_update = vfe_reg_update,
	.subdev_init = vfe_subdev_init,
	.vfe_disable = vfe_disable_17x,
	.vfe_enable = vfe_enable,
	.vfe_halt = vfe_halt,
	.violation_read = vfe_violation_read,
	.vfe_wm_start = vfe_wm_start,
	.vfe_wm_stop = vfe_wm_stop,
	.vfe_wm_update = vfe_wm_update,
	.stats_stop = vfe_17x_stats_stop,
};
