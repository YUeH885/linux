// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Linaro Ltd

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define IMX363_LINK_FREQ_678P4MHZ	678400000ULL
#define IMX363_LINK_FREQ_280MHZ		280000000ULL
#define IMX363_MCLK_FREQ_19P2MHZ		(19200 * HZ_PER_KHZ)
#define IMX363_DATA_LANES		4

#define IMX363_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX363_MODE_STANDBY		0x00
#define IMX363_MODE_STREAMING		0x01

#define IMX363_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX363_ORIENTATION_FACTORY	0x03

#define IMX363_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX363_CHIP_ID			0x0363

#define IMX363_REG_GROUP_HOLD		CCI_REG8(0x0104)

#define IMX363_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX363_EXPOSURE_MIN		8
#define IMX363_EXPOSURE_STEP		1
#define IMX363_EXPOSURE_MARGIN		10

#define IMX363_REG_ANALOGUE_GAIN	CCI_REG16(0x0204)
#define IMX363_ANALOGUE_GAIN_MIN	0
#define IMX363_ANALOGUE_GAIN_MAX	448
#define IMX363_ANALOGUE_GAIN_STEP	1
#define IMX363_ANALOGUE_GAIN_DEFAULT	0

#define IMX363_REG_DIGITAL_GAIN		CCI_REG16(0x020e)
#define IMX363_DIGITAL_GAIN_MIN		256
#define IMX363_DIGITAL_GAIN_MAX		4095
#define IMX363_DIGITAL_GAIN_STEP	1
#define IMX363_DIGITAL_GAIN_DEFAULT	256

#define IMX363_REG_VTS			CCI_REG16(0x0340)
#define IMX363_VTS_MAX			0xffff

#define to_imx363(_sd)			container_of(_sd, struct imx363, sd)

static const s64 imx363_link_freq_menu[] = {
	IMX363_LINK_FREQ_678P4MHZ,
	IMX363_LINK_FREQ_280MHZ,
};

/* The factory readout has both orientation bits enabled. */
static const u32 imx363_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
};

struct imx363_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct imx363_mode {
	u32 width;			/* Frame width in pixels */
	u32 height;			/* Frame height in pixels */
	u32 hts;			/* Horizontal timing size */
	u32 vts;			/* Default vertical timing size */
	u32 exposure;			/* Default exposure value */
	u64 pixel_rate;		/* VT timing clock, independent of CSI output clock */
	u8 link_freq_index;
	struct v4l2_rect crop;

	const struct imx363_reg_list reg_list;	/* Sensor register setting */
};

static const char * const imx363_supply_names[] = {
	"vddaux",	/* Module auxiliary power */
	"vdda",		/* Analogue power */
	"vddd",		/* Digital core power */
	"vddio",	/* Digital I/O power */
};

#define IMX363_NUM_SUPPLIES	ARRAY_SIZE(imx363_supply_names)

struct imx363 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX363_NUM_SUPPLIES];

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	/* Keep the controls adjacent for v4l2_ctrl_cluster(). */
	struct {
		struct v4l2_ctrl *exposure;
		struct v4l2_ctrl *analogue_gain;
	};
	struct v4l2_ctrl *digital_gain;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	const struct imx363_mode *mode;
};

static const struct cci_reg_sequence imx363_init_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x31a3), 0x00 },
	{ CCI_REG8(0x64d4), 0x01 },
	{ CCI_REG8(0x64d5), 0xaa },
	{ CCI_REG8(0x64d6), 0x01 },
	{ CCI_REG8(0x64d7), 0xa9 },
	{ CCI_REG8(0x64d8), 0x01 },
	{ CCI_REG8(0x64d9), 0xa5 },
	{ CCI_REG8(0x64da), 0x01 },
	{ CCI_REG8(0x64db), 0xa1 },
	{ CCI_REG8(0x720a), 0x24 },
	{ CCI_REG8(0x720b), 0x89 },
	{ CCI_REG8(0x720c), 0x85 },
	{ CCI_REG8(0x720d), 0xa1 },
	{ CCI_REG8(0x720e), 0x6e },
	{ CCI_REG8(0x729c), 0x59 },
	{ CCI_REG8(0x817c), 0xff },
	{ CCI_REG8(0x817d), 0x80 },
	{ CCI_REG8(0x9348), 0x96 },
	{ CCI_REG8(0x934b), 0x8c },
	{ CCI_REG8(0x934c), 0x82 },
	{ CCI_REG8(0x9353), 0xaa },
	{ CCI_REG8(0x9354), 0xaa },
	{ CCI_REG8(0x0101), 0x03 },
};

static const struct cci_reg_sequence imx363_4032x3024_30fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0340), 0x0c },
	{ CCI_REG8(0x0341), 0x54 },
	{ CCI_REG8(0x0342), 0x22 },
	{ CCI_REG8(0x0343), 0x80 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x30f4), 0x02 },
	{ CCI_REG8(0x30f5), 0x80 },
	{ CCI_REG8(0x30f6), 0x00 },
	{ CCI_REG8(0x30f7), 0xc8 },
	{ CCI_REG8(0x31a0), 0x00 },
	{ CCI_REG8(0x3040), 0x01 },
	{ CCI_REG8(0x31a5), 0x00 },
	{ CCI_REG8(0x31a6), 0x00 },
	{ CCI_REG8(0x560f), 0xbe },
	{ CCI_REG8(0x5856), 0x08 },
	{ CCI_REG8(0x58d0), 0x10 },
	{ CCI_REG8(0x734a), 0x01 },
	{ CCI_REG8(0x734f), 0x2b },
	{ CCI_REG8(0x7441), 0x55 },
	{ CCI_REG8(0x7914), 0x03 },
	{ CCI_REG8(0x7928), 0x04 },
	{ CCI_REG8(0x7929), 0x04 },
	{ CCI_REG8(0x793f), 0x03 },
	{ CCI_REG8(0xbc7b), 0x18 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x0f },
	{ CCI_REG8(0x0349), 0xbf },
	{ CCI_REG8(0x034a), 0x0b },
	{ CCI_REG8(0x034b), 0xcf },
	{ CCI_REG8(0x034c), 0x0f },
	{ CCI_REG8(0x034d), 0xc0 },
	{ CCI_REG8(0x034e), 0x0b },
	{ CCI_REG8(0x034f), 0xd0 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0f },
	{ CCI_REG8(0x040d), 0xc0 },
	{ CCI_REG8(0x040e), 0x0b },
	{ CCI_REG8(0x040f), 0xd0 },
	{ CCI_REG8(0x0301), 0x03 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xc4 },
	{ CCI_REG8(0x0309), 0x0a },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd4 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0202), 0x0c },
	{ CCI_REG8(0x0203), 0x44 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0226), 0x01 },
	{ CCI_REG8(0x0227), 0x00 },
};

static const struct cci_reg_sequence imx363_2016x1136_60fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0340), 0x04 },
	{ CCI_REG8(0x0341), 0xd4 },
	{ CCI_REG8(0x0342), 0x15 },
	{ CCI_REG8(0x0343), 0x90 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x30f4), 0x01 },
	{ CCI_REG8(0x30f5), 0xf4 },
	{ CCI_REG8(0x30f6), 0x00 },
	{ CCI_REG8(0x30f7), 0x14 },
	{ CCI_REG8(0x31a0), 0x00 },
	{ CCI_REG8(0x31a5), 0x00 },
	{ CCI_REG8(0x31a6), 0x01 },
	{ CCI_REG8(0x560f), 0x2a },
	{ CCI_REG8(0x5856), 0x08 },
	{ CCI_REG8(0x58d0), 0x10 },
	{ CCI_REG8(0x734a), 0x01 },
	{ CCI_REG8(0x734f), 0x2b },
	{ CCI_REG8(0x7441), 0x55 },
	{ CCI_REG8(0x7914), 0x03 },
	{ CCI_REG8(0x7928), 0x04 },
	{ CCI_REG8(0x7929), 0x04 },
	{ CCI_REG8(0x793f), 0x03 },
	{ CCI_REG8(0xbc7b), 0x18 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0x78 },
	{ CCI_REG8(0x0348), 0x0f },
	{ CCI_REG8(0x0349), 0xbf },
	{ CCI_REG8(0x034a), 0x0a },
	{ CCI_REG8(0x034b), 0x57 },
	{ CCI_REG8(0x034c), 0x07 },
	{ CCI_REG8(0x034d), 0xe0 },
	{ CCI_REG8(0x034e), 0x04 },
	{ CCI_REG8(0x034f), 0x70 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x07 },
	{ CCI_REG8(0x040d), 0xe0 },
	{ CCI_REG8(0x040e), 0x04 },
	{ CCI_REG8(0x040f), 0x70 },
	{ CCI_REG8(0x0301), 0x03 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x60 },
	{ CCI_REG8(0x0309), 0x0a },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xaf },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0202), 0x04 },
	{ CCI_REG8(0x0203), 0xc4 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0226), 0x01 },
	{ CCI_REG8(0x0227), 0x00 },
};

static const struct imx363_mode imx363_supported_modes[] = {
	{
		.width = 4032,
		.height = 3024,
		.hts = 8832,
		.vts = 3156,
		.exposure = 3140,
		.pixel_rate = 836266667,
		.link_freq_index = 0,
		.crop = { 0, 0, 4032, 3024 },
		.reg_list = {
			.regs = imx363_4032x3024_30fps_regs,
			.num_regs = ARRAY_SIZE(imx363_4032x3024_30fps_regs),
		},
	},
	{
		.width = 2016,
		.height = 1136,
		.hts = 5520,
		.vts = 1236,
		.exposure = 1220,
		.pixel_rate = 409600000,
		.link_freq_index = 1,
		.crop = { 0, 376, 4032, 2272 },
		.reg_list = {
			.regs = imx363_2016x1136_60fps_regs,
			.num_regs = ARRAY_SIZE(imx363_2016x1136_60fps_regs),
		},
	},
};

static int imx363_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx363 *imx363 = container_of(ctrl->handler, struct imx363,
					     ctrl_handler);
	const struct imx363_mode *mode = imx363->mode;
	s64 exposure_max;
	int ret, release;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/* The orientation settings are applied along with streaming */
		return 0;
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		exposure_max = mode->height + ctrl->val - IMX363_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx363->exposure,
					 imx363->exposure->minimum,
					 exposure_max,
					 imx363->exposure->step,
					 imx363->exposure->default_value);
		break;
	}

	/* V4L2 controls are applied, when sensor is powered up for streaming */
	if (pm_runtime_get_if_in_use(imx363->dev) <= 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(imx363->regmap, IMX363_REG_DIGITAL_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx363->regmap, IMX363_REG_GROUP_HOLD, 1, NULL);
		cci_write(imx363->regmap, IMX363_REG_EXPOSURE,
			  imx363->exposure->val, &ret);
		cci_write(imx363->regmap, IMX363_REG_ANALOGUE_GAIN,
			  imx363->analogue_gain->val, &ret);
		release = cci_write(imx363->regmap, IMX363_REG_GROUP_HOLD, 0, NULL);
		if (!ret)
			ret = release;
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx363->regmap, IMX363_REG_VTS,
				ctrl->val + mode->height, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx363->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx363_ctrl_ops = {
	.s_ctrl = imx363_set_ctrl,
};

static int imx363_init_controls(struct imx363 *imx363)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx363->ctrl_handler;
	const struct imx363_mode *mode = imx363->mode;
	s64 pixel_rate, hblank, vblank, exposure_max;
	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 9);

	imx363->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(imx363_link_freq_menu) - 1,
						   mode->link_freq_index,
						   imx363_link_freq_menu);
	if (imx363->link_freq)
		imx363->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = mode->pixel_rate;
	imx363->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
					       V4L2_CID_PIXEL_RATE,
					       0, pixel_rate, 1, pixel_rate);

	hblank = mode->hts - mode->width;
	imx363->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (imx363->hblank)
		imx363->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	imx363->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   IMX363_VTS_MAX - mode->height, 1,
					   vblank);

	imx363->analogue_gain =
		v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				  IMX363_ANALOGUE_GAIN_MIN, IMX363_ANALOGUE_GAIN_MAX,
				  IMX363_ANALOGUE_GAIN_STEP, IMX363_ANALOGUE_GAIN_DEFAULT);

	imx363->digital_gain = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
						 V4L2_CID_DIGITAL_GAIN,
						 IMX363_DIGITAL_GAIN_MIN,
						 IMX363_DIGITAL_GAIN_MAX,
						 IMX363_DIGITAL_GAIN_STEP,
						 IMX363_DIGITAL_GAIN_DEFAULT);

	exposure_max = mode->vts - IMX363_EXPOSURE_MARGIN;
	imx363->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX363_EXPOSURE_MIN,
					     exposure_max,
					     IMX363_EXPOSURE_STEP,
					     mode->exposure);

	imx363->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx363->hflip)
		imx363->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx363->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx363->vflip)
		imx363->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(imx363->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx363_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}
	v4l2_ctrl_cluster(2, &imx363->exposure);

	imx363->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int imx363_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx363 *imx363 = to_imx363(sd);
	const struct imx363_reg_list *reg_list = &imx363->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(imx363->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(imx363->regmap, imx363_init_regs,
			    ARRAY_SIZE(imx363_init_regs), &ret);
	cci_multi_reg_write(imx363->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(imx363->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(imx363->regmap, IMX363_REG_ORIENTATION,
		  IMX363_ORIENTATION_FACTORY ^
		  (imx363->hflip->val ? BIT(0) : 0) ^
		  (imx363->vflip->val ? BIT(1) : 0), &ret);
	cci_write(imx363->regmap, IMX363_REG_MODE_SELECT,
		  IMX363_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(imx363->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(imx363->dev);

	return ret;
}

static int imx363_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	ret = cci_write(imx363->regmap, IMX363_REG_MODE_SELECT,
			IMX363_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx363->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(imx363->dev);

	return ret;
}

static u32 imx363_get_format_code(struct imx363 *imx363)
{
	unsigned int i;

	i = (imx363->vflip->val ? 2 : 0) | (imx363->hflip->val ? 1 : 0);

	return imx363_mbus_formats[i];
}

static void imx363_update_pad_format(struct imx363 *imx363,
				     const struct imx363_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = imx363_get_format_code(imx363);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx363_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	s64 hblank, vblank, exposure_max;
	const struct imx363_mode *mode;

	mode = v4l2_find_nearest_size(imx363_supported_modes,
				      ARRAY_SIZE(imx363_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	imx363_update_pad_format(imx363, mode, &fmt->format);

	/* Format code could be updated with respect to flip controls */
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || imx363->mode == mode)
		goto set_format;

	imx363->mode = mode;
	__v4l2_ctrl_s_ctrl(imx363->link_freq, mode->link_freq_index);
	__v4l2_ctrl_s_ctrl_int64(imx363->pixel_rate, mode->pixel_rate);

	/* Update limits and set FPS and exposure to default values */
	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(imx363->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(imx363->vblank, vblank,
				 IMX363_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(imx363->vblank, vblank);

	exposure_max = mode->vts - IMX363_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(imx363->exposure, IMX363_EXPOSURE_MIN,
				 exposure_max, IMX363_EXPOSURE_STEP,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(imx363->exposure, mode->exposure);

	if (imx363->sd.ctrl_handler->error)
		return imx363->sd.ctrl_handler->error;

set_format:
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	return 0;
}

static int imx363_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx363 *imx363 = to_imx363(sd);

	/* Media bus code index is constant, but code formats are not */
	if (code->index > 0)
		return -EINVAL;

	code->code = imx363_get_format_code(imx363);

	return 0;
}

static int imx363_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx363 *imx363 = to_imx363(sd);

	if (fse->index >= ARRAY_SIZE(imx363_supported_modes))
		return -EINVAL;

	if (fse->code != imx363_get_format_code(imx363))
		return -EINVAL;

	fse->min_width = imx363_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = imx363_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int imx363_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	const struct v4l2_mbus_framefmt *format;
	const struct imx363_mode *mode;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		format = v4l2_subdev_state_get_format(sd_state, 0);
		mode = v4l2_find_nearest_size(imx363_supported_modes,
					      ARRAY_SIZE(imx363_supported_modes),
					      width, height,
					      format->width, format->height);
		sel->r = mode->crop;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = imx363_supported_modes[0].crop;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx363_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			/* Media bus code depends on current flip controls */
			.width = imx363->mode->width,
			.height = imx363->mode->height,
		},
	};

	imx363_set_pad_format(sd, state, &fmt);

	return 0;
}

static const struct v4l2_subdev_video_ops imx363_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx363_pad_ops = {
	.set_fmt = imx363_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = imx363_get_selection,
	.enum_mbus_code = imx363_enum_mbus_code,
	.enum_frame_size = imx363_enum_frame_size,
	.enable_streams = imx363_enable_streams,
	.disable_streams = imx363_disable_streams,
};

static const struct v4l2_subdev_ops imx363_subdev_ops = {
	.video = &imx363_video_ops,
	.pad = &imx363_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx363_internal_ops = {
	.init_state = imx363_init_state,
};

static const struct media_entity_operations imx363_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx363_identify_sensor(struct imx363 *imx363)
{
	u64 val;
	int ret;

	ret = cci_read(imx363->regmap, IMX363_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(imx363->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != IMX363_CHIP_ID) {
		dev_err(imx363->dev, "chip id mismatch: %x!=%llx\n",
			IMX363_CHIP_ID, val);
		return -ENODEV;
	}

	return 0;
}

static int imx363_check_hwcfg(struct imx363 *imx363)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx363->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus = {
			.mipi_csi2 = {
				.num_data_lanes = IMX363_DATA_LANES,
			},
		},
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX363_DATA_LANES) {
		dev_err(imx363->dev, "Invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx363->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx363_link_freq_menu,
				       ARRAY_SIZE(imx363_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx363_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);
	unsigned int i;
	int ret;

	/* The factory sequence enables VAUX, VANA, VDIG and VIO in that order. */
	for (i = 0; i < IMX363_NUM_SUPPLIES; i++) {
		ret = regulator_enable(imx363->supplies[i].consumer);
		if (ret)
			goto disable_regulators;
		if (i == 0)
			usleep_range(1000, 2000);
	}
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(imx363->reset_gpio, 0);
	usleep_range(3000, 4000);

	ret = clk_prepare_enable(imx363->mclk);
	if (ret)
		goto assert_reset;
	usleep_range(10000, 11000);

	return 0;

assert_reset:
	gpiod_set_value_cansleep(imx363->reset_gpio, 1);
	usleep_range(1000, 2000);
disable_regulators:
	regulator_bulk_disable(i, imx363->supplies);

	return ret;
}

static int imx363_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	clk_disable_unprepare(imx363->mclk);
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(imx363->reset_gpio, 1);
	usleep_range(1000, 2000);

	/* Bulk disable walks the supplies in reverse: VIO, VDIG, VANA, VAUX. */
	ret = regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);

	return ret;
}

static int imx363_probe(struct i2c_client *client)
{
	struct imx363 *imx363;
	unsigned long freq;
	unsigned int i;
	int ret;

	imx363 = devm_kzalloc(&client->dev, sizeof(*imx363), GFP_KERNEL);
	if (!imx363)
		return -ENOMEM;

	imx363->dev = &client->dev;
	v4l2_i2c_subdev_init(&imx363->sd, client, &imx363_subdev_ops);

	imx363->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx363->regmap))
		return dev_err_probe(imx363->dev, PTR_ERR(imx363->regmap),
				     "failed to init CCI\n");

	imx363->mclk = devm_v4l2_sensor_clk_get(imx363->dev, NULL);
	if (IS_ERR(imx363->mclk))
		return dev_err_probe(imx363->dev, PTR_ERR(imx363->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(imx363->mclk);
	if (freq != IMX363_MCLK_FREQ_19P2MHZ)
		return dev_err_probe(imx363->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = imx363_check_hwcfg(imx363);
	if (ret)
		return dev_err_probe(imx363->dev, ret,
				     "failed to check HW configuration\n");

	imx363->reset_gpio = devm_gpiod_get_optional(imx363->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx363->reset_gpio))
		return dev_err_probe(imx363->dev, PTR_ERR(imx363->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < IMX363_NUM_SUPPLIES; i++)
		imx363->supplies[i].supply = imx363_supply_names[i];

	ret = devm_regulator_bulk_get(imx363->dev, IMX363_NUM_SUPPLIES,
				      imx363->supplies);
	if (ret)
		return dev_err_probe(imx363->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = imx363_power_on(imx363->dev);
	if (ret)
		return ret;

	ret = imx363_identify_sensor(imx363);
	if (ret) {
		dev_err_probe(imx363->dev, ret, "failed to find sensor\n");
		goto power_off;
	}

	imx363->mode = &imx363_supported_modes[0];
	ret = imx363_init_controls(imx363);
	if (ret) {
		dev_err_probe(imx363->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	imx363->sd.state_lock = imx363->ctrl_handler.lock;
	imx363->sd.internal_ops = &imx363_internal_ops;
	imx363->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx363->sd.entity.ops = &imx363_subdev_entity_ops;
	imx363->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx363->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx363->sd.entity, 1, &imx363->pad);
	if (ret) {
		dev_err_probe(imx363->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&imx363->sd);
	if (ret < 0) {
		dev_err_probe(imx363->dev, ret,
			      "failed to init media entity pads\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(imx363->dev);
	pm_runtime_enable(imx363->dev);

	ret = v4l2_async_register_subdev_sensor(&imx363->sd);
	if (ret < 0) {
		dev_err_probe(imx363->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(imx363->dev, 1000);
	pm_runtime_use_autosuspend(imx363->dev);
	pm_runtime_idle(imx363->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&imx363->sd);
	pm_runtime_disable(imx363->dev);
	pm_runtime_set_suspended(imx363->dev);

media_entity_cleanup:
	media_entity_cleanup(&imx363->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(imx363->sd.ctrl_handler);

power_off:
	imx363_power_off(imx363->dev);

	return ret;
}

static void imx363_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(imx363->dev);

	if (!pm_runtime_status_suspended(imx363->dev)) {
		imx363_power_off(imx363->dev);
		pm_runtime_set_suspended(imx363->dev);
	}
}

static const struct dev_pm_ops imx363_pm_ops = {
	SET_RUNTIME_PM_OPS(imx363_power_off, imx363_power_on, NULL)
};

static const struct of_device_id imx363_of_match[] = {
	{ .compatible = "sony,imx363" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx363_of_match);

static struct i2c_driver imx363_i2c_driver = {
	.driver = {
		.name = "imx363",
		.pm = &imx363_pm_ops,
		.of_match_table = imx363_of_match,
	},
	.probe = imx363_probe,
	.remove = imx363_remove,
};

module_i2c_driver(imx363_i2c_driver);

MODULE_DESCRIPTION("Sony IMX363 image sensor driver");
MODULE_LICENSE("GPL");
