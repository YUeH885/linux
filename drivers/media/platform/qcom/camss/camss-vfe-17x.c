// SPDX-License-Identifier: GPL-2.0
/*
 * camss-vfe-170.c
 *
 * Qualcomm MSM Camera Subsystem - VFE (Video Front End) Module v170
 *
 * Copyright (C) 2020-2021 Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>

#include <media/v4l2-event.h>

#include "camss.h"
#include "camss-vfe.h"

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
#define		STATUS_0_IMAGE_MASTER_PING_PONG(n)	BIT((n) + 8)
#define		STATUS_0_IMAGE_COMPOSITE_DONE(n)	BIT((n) + 25)
#define		STATUS_0_RESET_ACK			BIT(31)

#define VFE_IRQ_STATUS_1				(0x070)
#define		STATUS_1_VIOLATION			BIT(7)
#define		STATUS_1_BUS_BDG_HALT_ACK		BIT(8)
#define		STATUS_1_RDI_SOF(n)			BIT((n) + 27)

#define VFE_VIOLATION_STATUS			(0x07c)

#define VFE_MODULE_LENS_EN			(0x040)
#define		LENS_DEMUX_EN			BIT(2)
#define		LENS_DEMOSAIC_EN		BIT(10)

#define VFE_MODULE_COLOR_EN			(0x048)
#define		COLOR_CC_EN			BIT(1)

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


#define VFE_DEMO_CFG				(0x6f8)
#define VFE_DEMO_WB_LEFT_CFG_0			(0x6fc)
#define VFE_DEMO_WB_LEFT_CFG_1			(0x700)
#define VFE_DEMO_WB_LEFT_OFFSET_CFG_0		(0x704)
#define VFE_DEMO_WB_LEFT_OFFSET_CFG_1		(0x708)
#define VFE_DEMO_INTERP_COEFF_CFG		(0x71c)
#define VFE_DEMO_INTERP_CLASSIFIER_0		(0x720)

#define VFE_COLOR_CORRECT_COEFF_0		(0x760)
#define VFE_COLOR_CORRECT_OFFSET_0		(0x784)
#define VFE_COLOR_CORRECT_COEFF_Q		(0x790)

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
#define		FULL_OUT_CLAMP_MAX(n)		((n) << 20)
#define		FULL_OUT_ROUND_SHIFT(n)		((n) << 3)

#define VFE_COLOR_XFORM_CH0_COEFF_CFG_0		(0x0f30)

#define VFE_REG_UPDATE_CMD			(0x4ac)
#define		REG_UPDATE_RDI(n)		BIT(1 + (n))
#define		REG_UPDATE_PIX			BIT(0)
#define		REG_UPDATE_LINE(vfe, n)		\
	((n) == VFE_LINE_PIX && !vfe->res->is_lite ? \
	 REG_UPDATE_PIX : REG_UPDATE_RDI(n))
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

static void vfe_pix_wm_geometry(struct vfe_line *line, u8 wm, u32 *width,
				u32 *height, u32 *stride, u32 *frame_inc)
{
	struct vfe_output *output = &line->output;
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;

	*width = pix->width;
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

static int vfe_pix_program(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	int pattern;
	u32 first, last, top, bottom;
	u32 val;

	pattern = vfe_pix_pattern(sink->code);
	if (pattern < 0) {
		dev_err(vfe->camss->dev, "Unsupported Bayer format 0x%x\n",
			sink->code);
		return -EINVAL;
	}

	writel_relaxed(pattern, vfe->base + VFE_CORE_CFG);

	first = 0;
	last = sink->width - 1;
	top = 0;
	bottom = sink->height - 1;

	writel_relaxed((first << 16) | last,
		       vfe->base + VFE_CAMIF_RAW_CROP_WIDTH_CFG);
	writel_relaxed((top << 16) | bottom,
		       vfe->base + VFE_CAMIF_RAW_CROP_HEIGHT_CFG);

	writel_relaxed(CMD_CLEAR_CAMIF_STATUS, vfe->base + VFE_CAMIF_CMD);

	/* Demux 13 uses Q10 gains and Bayer even/odd line patterns. */
	writel_relaxed(1, vfe->base + VFE_DEMUX_CFG);
	writel_relaxed(0x04000400, vfe->base + VFE_DEMUX_CFG + 4);
	writel_relaxed(0x04000400, vfe->base + VFE_DEMUX_CFG + 8);
	writel_relaxed(0x04000400, vfe->base + VFE_DEMUX_CFG + 12);
	writel_relaxed(0x04000400, vfe->base + VFE_DEMUX_CFG + 16);
	writel_relaxed(0xc9, vfe->base + VFE_DEMUX_CFG + 20);
	writel_relaxed(0xac, vfe->base + VFE_DEMUX_CFG + 24);

	/* Unity white balance, identity CCM and linear demosaic. */
	writel_relaxed(0x00800080, vfe->base + VFE_DEMO_WB_LEFT_CFG_0);
	writel_relaxed(0x00000080, vfe->base + VFE_DEMO_WB_LEFT_CFG_1);
	writel_relaxed(0, vfe->base + VFE_DEMO_WB_LEFT_OFFSET_CFG_0);
	writel_relaxed(0, vfe->base + VFE_DEMO_WB_LEFT_OFFSET_CFG_1);

	writel_relaxed(0x100, vfe->base + VFE_DEMO_CFG);
	writel_relaxed(0x00008000, vfe->base + VFE_DEMO_INTERP_COEFF_CFG);
	writel_relaxed(0x08000066, vfe->base + VFE_DEMO_INTERP_CLASSIFIER_0);

	writel_relaxed(0x00000080, vfe->base + VFE_COLOR_CORRECT_COEFF_0);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 4);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 8);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 12);
	writel_relaxed(0x00000080, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 16);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 20);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 24);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 28);
	writel_relaxed(0x00000080, vfe->base + VFE_COLOR_CORRECT_COEFF_0 + 32);
	writel_relaxed(0, vfe->base + VFE_COLOR_CORRECT_OFFSET_0);
	writel_relaxed(0, vfe->base + VFE_COLOR_CORRECT_OFFSET_0 + 4);
	writel_relaxed(0, vfe->base + VFE_COLOR_CORRECT_OFFSET_0 + 8);
	writel_relaxed(0, vfe->base + VFE_COLOR_CORRECT_COEFF_Q);

	/* RGB to BT.601 YUV, Q10 coefficients. */
	writel_relaxed(0x00750259, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0);
	writel_relaxed(0x00000132, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 4);
	writel_relaxed(0x00000000, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 8);
	writel_relaxed(0x03ff0000, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 12);
	writel_relaxed(0x01fe1eae, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 16);
	writel_relaxed(0x00001f54, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 20);
	writel_relaxed(0x02000000, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 24);
	writel_relaxed(0x03ff0000, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 28);
	writel_relaxed(0x1fad1e55, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 32);
	writel_relaxed(0x000001fe, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 36);
	writel_relaxed(0x02000000, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 40);
	writel_relaxed(0x03ff0000, vfe->base + VFE_COLOR_XFORM_CH0_COEFF_CFG_0 + 44);

	vfe_pix_scale(vfe, VFE_FULL_SCALE_Y_CFG, sink->width, sink->height,
		      line->compose.width, line->compose.height);
	vfe_pix_scale(vfe, VFE_FULL_SCALE_C_CFG, sink->width, sink->height,
		      line->compose.width / 2, line->compose.height / 2);

	first = line->crop.left;
	last = first + line->crop.width - 1;
	top = line->crop.top;
	bottom = top + line->crop.height - 1;

	writel_relaxed((first << 16) | last,
		       vfe->base + VFE_FULL_OUT_Y_CROP_PIXEL_CFG);
	writel_relaxed((top << 16) | bottom,
		       vfe->base + VFE_FULL_OUT_Y_CROP_LINE_CFG);
	/* Chroma crop counts interleaved components horizontally. */
	writel_relaxed((first << 16) | last,
		       vfe->base + VFE_FULL_OUT_C_CROP_PIXEL_CFG);
	writel_relaxed((top / 2 << 16) | (bottom / 2),
		       vfe->base + VFE_FULL_OUT_C_CROP_LINE_CFG);
	writel_relaxed(FULL_OUT_CLAMP_MAX(255),
		       vfe->base + VFE_FULL_OUT_Y_CH0_CLAMP_CFG);
	writel_relaxed(FULL_OUT_CLAMP_MAX(255),
		       vfe->base + VFE_FULL_OUT_C_CH0_CLAMP_CFG);
	writel_relaxed(FULL_OUT_ROUND_SHIFT(2) | 6,
		       vfe->base + VFE_FULL_OUT_Y_ROUND_CFG);
	writel_relaxed(FULL_OUT_ROUND_SHIFT(2) | 7,
		       vfe->base + VFE_FULL_OUT_C_ROUND_CFG);
	val = FULL_OUT_CROP_EN | FULL_OUT_ROUND_EN | FULL_OUT_CLAMP_EN;
	writel_relaxed(val, vfe->base + VFE_FULL_OUT_Y_CROP_RND_CLAMP_CFG);
	writel_relaxed(val, vfe->base + VFE_FULL_OUT_C_CROP_RND_CLAMP_CFG);

	val = LENS_DEMUX_EN | LENS_DEMOSAIC_EN;
	writel_relaxed(val, vfe->base + VFE_MODULE_LENS_EN);
	writel_relaxed(COLOR_CC_EN, vfe->base + VFE_MODULE_COLOR_EN);
	val = ZOOM_CST_EN | ZOOM_SCALE_VID_EN | ZOOM_CROP_VID_EN;
	writel_relaxed(val, vfe->base + VFE_MODULE_ZOOM_EN);

	writel_relaxed(CFG_VFE_OUTPUT_EN | CFG_BUS_OUTPUT_EN | CFG_RAW_CROP_EN,
		       vfe->base + VFE_CAMIF_CFG);
	writel_relaxed(0xffff, vfe->base + VFE_CAMIF_LINE_SKIP_PATTERN);
	writel_relaxed(0xffff, vfe->base + VFE_CAMIF_PIXEL_SKIP_PATTERN);
	writel_relaxed(0, vfe->base + VFE_CAMIF_SKIP_PERIOD);
	writel_relaxed(~0U, vfe->base + VFE_CAMIF_IRQ_SUBSAMPLE_PATTERN);
	writel_relaxed(BIT(3) | BIT(4), vfe->base + VFE_BUS_COMP_GRP_0);

	return 0;
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

static void vfe_wm_start(struct vfe_device *vfe, u8 wm, struct vfe_line *line)
{
	u32 val;

	/*Set Debug Registers*/
	val = DEBUG_STATUS_CFG_STATUS0(1) |
	      DEBUG_STATUS_CFG_STATUS0(7);
	writel_relaxed(val, vfe->base + VFE_BUS_WM_DEBUG_STATUS_CFG);

	/* BUS_WM_INPUT_IF_ADDR_SYNC_FRAME_HEADER */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_ADDR_SYNC_FRAME_HEADER);

	/* no clock gating at bus input */
	val = WM_CGC_OVERRIDE_ALL;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_CGC_OVERRIDE);

	writel_relaxed(0x0, vfe->base + VFE_BUS_WM_TEST_BUS_CTRL);

	/* if addr_no_sync has default value then config the addr no sync reg */
	val = WM_ADDR_NO_SYNC_DEFAULT_VAL;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_ADDR_SYNC_NO_SYNC);

	writel_relaxed(0xf, vfe->base + VFE_BUS_WM_BURST_LIMIT(wm));

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		u32 width, height, stride, frame_inc;

		vfe_pix_wm_geometry(line, wm, &width, &height, &stride,
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
		dev_dbg(vfe->camss->dev,
			 "WM%d cfg=0x%x w=%u h=%u pack=0x%x stride=%u inc=%u\n",
			 wm, readl_relaxed(vfe->base + VFE_BUS_WM_CFG(wm)),
			 readl_relaxed(vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm)),
			 readl_relaxed(vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm)),
			 readl_relaxed(vfe->base + VFE_BUS_WM_PACKER_CFG(wm)),
			 readl_relaxed(vfe->base + VFE_BUS_WM_STRIDE(wm)),
			 readl_relaxed(vfe->base + VFE_BUS_WM_FRAME_INC(wm)));
		return;
	}

	val = WM_BUFFER_DEFAULT_WIDTH;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));

	val = 0;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));

	val = 0;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_PACKER_CFG(wm)); // XXX 1 for PLAIN8?

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
	/* Disable WM */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_CFG(wm));
}

static void vfe_wm_update(struct vfe_device *vfe, u8 wm, u32 addr,
			  struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;
	u32 stride = pix->plane_fmt[0].bytesperline;
	u32 width, height, plane_stride, frame_inc;

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		vfe_pix_wm_geometry(line, wm, &width, &height, &plane_stride,
				    &frame_inc);
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
	} else {
		writel_relaxed(stride * pix->height,
			       vfe->base + VFE_BUS_WM_FRAME_INC(wm));
	}
}

static void vfe_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	vfe->reg_update |= REG_UPDATE_LINE(vfe, line_id);

	/* Enforce ordering between previous reg writes and reg update */
	wmb();

	writel_relaxed(vfe->reg_update, vfe->base + VFE_REG_UPDATE_CMD);

	/* Enforce ordering between reg update and subsequent reg writes */
	wmb();
}

static inline void vfe_reg_update_clear(struct vfe_device *vfe,
					enum vfe_line_id line_id)
{
	vfe->reg_update &= ~REG_UPDATE_LINE(vfe, line_id);
}

static void vfe_enable_irq_common(struct vfe_device *vfe)
{
	u32 mask = STATUS_0_RESET_ACK;
	unsigned int i;

	for (i = 0; i < (vfe->res->is_lite ? 4 : 3); i++)
		mask |= STATUS_0_RDI_REG_UPDATE(i) | STATUS_1_RDI_SOF(i);
	if (!vfe->res->is_lite)
		mask |= STATUS_0_CAMIF_SOF | STATUS_0_PIX_REG_UPDATE;
	writel_relaxed(mask, vfe->base + VFE_IRQ_MASK_0);
	writel_relaxed(STATUS_1_VIOLATION, vfe->base + VFE_IRQ_MASK_1);
	writel_relaxed(STATUS0_COMP0_BUF_DONE | STATUS0_COMP_ERROR |
		       STATUS0_COMP_OVERWRITE | STATUS0_OVERFLOW | STATUS0_VIOLATION,
		       vfe->base + VFE_BUS_IRQ_MASK(0));
	writel_relaxed(vfe->res->is_lite ? GENMASK(3, 0) : GENMASK(2, 0),
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
	int i, wm;

	status0 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_0);
	status1 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_1);

	writel_relaxed(status0, vfe->base + VFE_IRQ_CLEAR_0);
	writel_relaxed(status1, vfe->base + VFE_IRQ_CLEAR_1);

	for (i = VFE_LINE_RDI0; i < 3; i++) {
		vfe_bus_status[i] = readl_relaxed(vfe->base + VFE_BUS_IRQ_STATUS(i));
		writel_relaxed(vfe_bus_status[i], vfe->base + VFE_BUS_IRQ_CLEAR(i));
	}

	/* Enforce ordering between IRQ reading and interpretation */
	wmb();

	writel_relaxed(CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);
	writel_relaxed(1, vfe->base + VFE_BUS_IRQ_CLEAR_GLOBAL);

	dev_dbg(vfe->camss->dev, "VFE%u IRQ top=%08x/%08x bus=%08x/%08x/%08x\n",
		vfe->id, status0, status1, vfe_bus_status[0],
		vfe_bus_status[1], vfe_bus_status[2]);
	if (status1 & STATUS_1_VIOLATION)
		vfe_violation_read(vfe);
	if (vfe_bus_status[0] & (STATUS0_COMP_ERROR | STATUS0_COMP_OVERWRITE |
				 STATUS0_OVERFLOW | STATUS0_VIOLATION))
		dev_err_ratelimited(vfe->camss->dev,
			"VFE%u bus error %08x comp=%08x overwrite=%08x\n",
			vfe->id, vfe_bus_status[0],
			readl_relaxed(vfe->base + VFE_BUS_COMP_ERROR_STATUS),
			readl_relaxed(vfe->base + VFE_BUS_COMP_OVERWRITE_STATUS));

	if (status0 & STATUS_0_RESET_ACK)
		vfe->isr_ops.reset_ack(vfe);

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

	if (!vfe->res->is_lite &&
	    (vfe_bus_status[0] & STATUS0_COMP0_BUF_DONE))
		vfe->isr_ops.wm_done(vfe, 3);

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

	if (!vfe->stream_count)
		vfe_enable_irq_common(vfe);

	vfe->stream_count++;

	mutex_unlock(&vfe->stream_lock);

	ret = vfe_get_output(line);
	if (ret < 0)
		goto error_get_output;

	if (line->id == VFE_LINE_PIX && !vfe->res->is_lite) {
		ret = vfe_pix_program(line);
		if (ret < 0)
			goto error_enable_output;
	}

	ret = vfe_enable_output_v2(line);
	if (ret < 0)
		goto error_enable_output;

	vfe->was_streaming = 1;

	return 0;

error_enable_output:
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

	spin_lock_irqsave(&vfe->output_lock, flags);
	if (output->state == VFE_OUTPUT_ON) {
		event.u.frame_sync.frame_sequence = output->sof_sequence++;
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
	unsigned int i;

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (vfe->wm_output_map[wm] == VFE_LINE_NONE) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Received wm done for unmapped index\n");
		goto out_unlock;
	}
	line = &vfe->line[vfe->wm_output_map[wm]];
	output = &line->output;

	ready_buf = output->buf[0];
	if (!ready_buf) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Missing ready buf %d!\n", output->state);
		goto out_unlock;
	}

	ready_buf->vb.vb2_buf.timestamp = ts;
	ready_buf->vb.sequence = output->sof_sequence - 1;

	index = 0;
	output->buf[0] = output->buf[1];
	if (output->buf[0])
		index = 1;

	output->buf[index] = vfe_buf_get_pending(output);

	if (output->buf[index]) {
		for (i = 0; i < output->wm_num; i++)
			vfe_wm_update(vfe, output->wm_idx[i],
				      output->buf[index]->addr[i], line);
	} else {
		output->gen2.active_num--;
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	vb2_buffer_done(&ready_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

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

static const struct camss_video_ops vfe_video_ops_170 = {
	.queue_buffer = vfe_queue_buffer_v2,
	.flush_buffers = vfe_flush_buffers,
};

static void vfe_subdev_init(struct device *dev, struct vfe_device *vfe)
{
	vfe->isr_ops = vfe_isr_ops_170;
	vfe->video_ops = vfe_video_ops_170;
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
	.vfe_disable = vfe_disable,
	.vfe_enable = vfe_enable,
	.vfe_halt = vfe_halt,
	.violation_read = vfe_violation_read,
	.vfe_wm_start = vfe_wm_start,
	.vfe_wm_stop = vfe_wm_stop,
	.vfe_wm_update = vfe_wm_update,
};
