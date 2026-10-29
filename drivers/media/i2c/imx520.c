// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 The LM-G820N Linux contributors

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

#define IMX520_LINK_FREQ_716P8MHZ	716800000ULL
#define IMX520_LINK_FREQ_675P2MHZ	675200000ULL
#define IMX520_MCLK_FREQ_19P2MHZ		(19200 * HZ_PER_KHZ)
#define IMX520_DATA_LANES		2

#define IMX520_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX520_MODE_STANDBY		0x00
#define IMX520_MODE_STREAMING		0x01

#define IMX520_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX520_ORIENTATION_FACTORY	0x02

#define IMX520_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX520_CHIP_ID			0x0520

#define IMX520_REG_GROUP_HOLD		CCI_REG8(0x0104)

#define IMX520_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX520_EXPOSURE_MIN		8
#define IMX520_EXPOSURE_STEP		1
#define IMX520_EXPOSURE_MARGIN		10

#define IMX520_REG_ANALOGUE_GAIN	CCI_REG16(0x0204)
#define IMX520_ANALOGUE_GAIN_MIN	0
#define IMX520_ANALOGUE_GAIN_MAX	960
#define IMX520_ANALOGUE_GAIN_STEP	1
#define IMX520_ANALOGUE_GAIN_DEFAULT	0

#define IMX520_REG_DIGITAL_GAIN		CCI_REG16(0x020e)
#define IMX520_DIGITAL_GAIN_MIN		256
#define IMX520_DIGITAL_GAIN_MAX		4095
#define IMX520_DIGITAL_GAIN_STEP	1
#define IMX520_DIGITAL_GAIN_DEFAULT	256

#define IMX520_REG_VTS			CCI_REG16(0x0340)
#define IMX520_VTS_MAX			0xffff

#define to_imx520(_sd)			container_of(_sd, struct imx520, sd)

static const s64 imx520_link_freq_menu[] = {
	IMX520_LINK_FREQ_716P8MHZ,
	IMX520_LINK_FREQ_675P2MHZ,
};

/* The factory readout has vertical orientation bit enabled. */
static const u32 imx520_mbus_formats[] = {
	MEDIA_BUS_FMT_SGBRG10_1X10,	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
};

struct imx520_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct imx520_mode {
	u32 width;			/* Frame width in pixels */
	u32 height;			/* Frame height in pixels */
	u32 hts;			/* Horizontal timing size */
	u32 vts;			/* Default vertical timing size */
	u32 exposure;			/* Default exposure value */
	u64 pixel_rate;
	u8 link_freq_index;
	struct v4l2_rect crop;

	const struct imx520_reg_list reg_list;	/* Sensor register setting */
};

static const char * const imx520_supply_names[] = {
	"vddio",	/* Digital I/O power */
	"vdda",		/* Analogue power */
	"vddd",		/* Digital core power */
};

#define IMX520_NUM_SUPPLIES	ARRAY_SIZE(imx520_supply_names)

struct imx520 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *mclk_request_gpio;
	struct regulator_bulk_data supplies[IMX520_NUM_SUPPLIES];

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

	const struct imx520_mode *mode;
};

static const struct cci_reg_sequence imx520_init_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x0808), 0x02 },
	{ CCI_REG8(0x3420), 0x01 },
	{ CCI_REG8(0x3e6a), 0x00 },
	{ CCI_REG8(0x3e7a), 0x01 },
	{ CCI_REG8(0x42b3), 0x00 },
	{ CCI_REG8(0x42cb), 0x00 },
	{ CCI_REG8(0x4421), 0x04 },
	{ CCI_REG8(0x5300), 0x00 },
	{ CCI_REG8(0x5602), 0x00 },
	{ CCI_REG8(0x5603), 0x94 },
	{ CCI_REG8(0x5c0c), 0x00 },
	{ CCI_REG8(0x5d8a), 0x00 },
	{ CCI_REG8(0x5d8d), 0x00 },
	{ CCI_REG8(0x5d90), 0x00 },
	{ CCI_REG8(0x5d93), 0x00 },
	{ CCI_REG8(0x5f04), 0x01 },
	{ CCI_REG8(0x600a), 0x02 },
	{ CCI_REG8(0x600c), 0x07 },
	{ CCI_REG8(0x6105), 0x40 },
	{ CCI_REG8(0x6106), 0xc7 },
	{ CCI_REG8(0x6107), 0x74 },
	{ CCI_REG8(0x6109), 0xba },
	{ CCI_REG8(0x610b), 0xdd },
	{ CCI_REG8(0x610d), 0xee },
	{ CCI_REG8(0x610e), 0x8c },
	{ CCI_REG8(0x610f), 0x77 },
	{ CCI_REG8(0x6110), 0x46 },
	{ CCI_REG8(0x6111), 0x3b },
	{ CCI_REG8(0x6112), 0xa4 },
	{ CCI_REG8(0x6113), 0x05 },
	{ CCI_REG8(0x6114), 0xd2 },
	{ CCI_REG8(0x6115), 0x02 },
	{ CCI_REG8(0x6116), 0xe8 },
	{ CCI_REG8(0x6118), 0x00 },
	{ CCI_REG8(0x6119), 0x20 },
	{ CCI_REG8(0x611a), 0x20 },
	{ CCI_REG8(0x611b), 0x38 },
	{ CCI_REG8(0x611c), 0x60 },
	{ CCI_REG8(0x611d), 0x70 },
	{ CCI_REG8(0x611e), 0xc0 },
	{ CCI_REG8(0x611f), 0x78 },
	{ CCI_REG8(0x6120), 0xc0 },
	{ CCI_REG8(0x6121), 0x00 },
	{ CCI_REG8(0x6122), 0x00 },
	{ CCI_REG8(0x6123), 0x20 },
	{ CCI_REG8(0x6124), 0x20 },
	{ CCI_REG8(0x6125), 0x3c },
	{ CCI_REG8(0x6126), 0x60 },
	{ CCI_REG8(0x6127), 0x70 },
	{ CCI_REG8(0x6128), 0xc0 },
	{ CCI_REG8(0x6130), 0x00 },
	{ CCI_REG8(0x6131), 0x00 },
	{ CCI_REG8(0x6132), 0x00 },
	{ CCI_REG8(0x6133), 0x2c },
	{ CCI_REG8(0x6134), 0x00 },
	{ CCI_REG8(0x6135), 0x50 },
	{ CCI_REG8(0x6136), 0x00 },
	{ CCI_REG8(0x6137), 0x90 },
	{ CCI_REG8(0x6138), 0x00 },
	{ CCI_REG8(0x6e25), 0xd4 },
	{ CCI_REG8(0x6e28), 0x01 },
	{ CCI_REG8(0x6f19), 0x00 },
	{ CCI_REG8(0x6f27), 0x00 },
	{ CCI_REG8(0x6f2f), 0x00 },
	{ CCI_REG8(0x6f4b), 0x01 },
	{ CCI_REG8(0x6f51), 0x02 },
	{ CCI_REG8(0x9008), 0x01 },
	{ CCI_REG8(0x9306), 0x00 },
	{ CCI_REG8(0x9307), 0x1c },
	{ CCI_REG8(0x9308), 0x00 },
	{ CCI_REG8(0x9309), 0x1a },
	{ CCI_REG8(0x930a), 0x00 },
	{ CCI_REG8(0x930b), 0x13 },
	{ CCI_REG8(0x930c), 0x00 },
	{ CCI_REG8(0x930d), 0x0b },
	{ CCI_REG8(0x930e), 0x00 },
	{ CCI_REG8(0x930f), 0x21 },
	{ CCI_REG8(0x9310), 0x00 },
	{ CCI_REG8(0x9311), 0x1b },
	{ CCI_REG8(0x9312), 0x00 },
	{ CCI_REG8(0x9313), 0x1a },
	{ CCI_REG8(0x9314), 0x00 },
	{ CCI_REG8(0x9315), 0x12 },
	{ CCI_REG8(0x931e), 0x00 },
	{ CCI_REG8(0x931f), 0x00 },
	{ CCI_REG8(0x9320), 0x00 },
	{ CCI_REG8(0x9321), 0x00 },
	{ CCI_REG8(0x932e), 0x00 },
	{ CCI_REG8(0x932f), 0x35 },
	{ CCI_REG8(0x9330), 0x00 },
	{ CCI_REG8(0x9331), 0x2a },
	{ CCI_REG8(0x9332), 0x00 },
	{ CCI_REG8(0x9333), 0x23 },
	{ CCI_REG8(0x9334), 0x00 },
	{ CCI_REG8(0x9335), 0x1f },
	{ CCI_REG8(0x933e), 0x00 },
	{ CCI_REG8(0x933f), 0x00 },
	{ CCI_REG8(0x9340), 0x00 },
	{ CCI_REG8(0x9341), 0x00 },
	{ CCI_REG8(0xbc42), 0x00 },
	{ CCI_REG8(0xbc43), 0x00 },
	{ CCI_REG8(0xe07a), 0x01 },
	{ CCI_REG8(0x0101), 0x02 },
};

static const struct cci_reg_sequence imx520_3280x2460_30fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0xb6 },
	{ CCI_REG8(0x0342), 0x0f },
	{ CCI_REG8(0x0343), 0x20 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x01 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x32 },
	{ CCI_REG8(0x5c8e), 0x0e },
	{ CCI_REG8(0x5ddd), 0x0e },
	{ CCI_REG8(0x5e0e), 0x0e },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x0c },
	{ CCI_REG8(0x0349), 0xcf },
	{ CCI_REG8(0x034a), 0x09 },
	{ CCI_REG8(0x034b), 0x9f },
	{ CCI_REG8(0x034c), 0x0c },
	{ CCI_REG8(0x034d), 0xd0 },
	{ CCI_REG8(0x034e), 0x09 },
	{ CCI_REG8(0x034f), 0x9c },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x02 },
	{ CCI_REG8(0x040c), 0x0c },
	{ CCI_REG8(0x040d), 0xd0 },
	{ CCI_REG8(0x040e), 0x09 },
	{ CCI_REG8(0x040f), 0x9c },
	{ CCI_REG8(0x0301), 0x09 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0xc0 },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xe6 },
	{ CCI_REG8(0x0310), 0x00 },
	{ CCI_REG8(0x080a), 0x00 },
	{ CCI_REG8(0x080b), 0x8f },
	{ CCI_REG8(0x080c), 0x00 },
	{ CCI_REG8(0x080d), 0x5f },
	{ CCI_REG8(0x080e), 0x00 },
	{ CCI_REG8(0x080f), 0x97 },
	{ CCI_REG8(0x0810), 0x00 },
	{ CCI_REG8(0x0811), 0x6f },
	{ CCI_REG8(0x0812), 0x00 },
	{ CCI_REG8(0x0813), 0x67 },
	{ CCI_REG8(0x0814), 0x00 },
	{ CCI_REG8(0x0815), 0x57 },
	{ CCI_REG8(0x0816), 0x01 },
	{ CCI_REG8(0x0817), 0x77 },
	{ CCI_REG8(0x0818), 0x00 },
	{ CCI_REG8(0x0819), 0x4f },
	{ CCI_REG8(0xe04c), 0x00 },
	{ CCI_REG8(0xe04d), 0x97 },
	{ CCI_REG8(0xe04e), 0x00 },
	{ CCI_REG8(0xe04f), 0x1f },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0xac },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct cci_reg_sequence imx520_3280x1848_30fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0x16 },
	{ CCI_REG8(0x0342), 0x0f },
	{ CCI_REG8(0x0343), 0x20 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x01 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x32 },
	{ CCI_REG8(0x5c8e), 0x0e },
	{ CCI_REG8(0x5ddd), 0x0e },
	{ CCI_REG8(0x5e0e), 0x0e },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0x30 },
	{ CCI_REG8(0x0348), 0x0c },
	{ CCI_REG8(0x0349), 0xcf },
	{ CCI_REG8(0x034a), 0x08 },
	{ CCI_REG8(0x034b), 0x6f },
	{ CCI_REG8(0x034c), 0x0c },
	{ CCI_REG8(0x034d), 0xd0 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x38 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x04 },
	{ CCI_REG8(0x040c), 0x0c },
	{ CCI_REG8(0x040d), 0xd0 },
	{ CCI_REG8(0x040e), 0x07 },
	{ CCI_REG8(0x040f), 0x38 },
	{ CCI_REG8(0x0301), 0x09 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0xa6 },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xe6 },
	{ CCI_REG8(0x0310), 0x00 },
	{ CCI_REG8(0x080a), 0x00 },
	{ CCI_REG8(0x080b), 0x8f },
	{ CCI_REG8(0x080c), 0x00 },
	{ CCI_REG8(0x080d), 0x57 },
	{ CCI_REG8(0x080e), 0x00 },
	{ CCI_REG8(0x080f), 0x8f },
	{ CCI_REG8(0x0810), 0x00 },
	{ CCI_REG8(0x0811), 0x67 },
	{ CCI_REG8(0x0812), 0x00 },
	{ CCI_REG8(0x0813), 0x67 },
	{ CCI_REG8(0x0814), 0x00 },
	{ CCI_REG8(0x0815), 0x57 },
	{ CCI_REG8(0x0816), 0x01 },
	{ CCI_REG8(0x0817), 0x5f },
	{ CCI_REG8(0x0818), 0x00 },
	{ CCI_REG8(0x0819), 0x47 },
	{ CCI_REG8(0xe04c), 0x00 },
	{ CCI_REG8(0xe04d), 0x8f },
	{ CCI_REG8(0xe04e), 0x00 },
	{ CCI_REG8(0xe04f), 0x1f },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0x0c },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct cci_reg_sequence imx520_1640x1232_30fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0x16 },
	{ CCI_REG8(0x0342), 0x0f },
	{ CCI_REG8(0x0343), 0x20 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x03 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x00 },
	{ CCI_REG8(0x5c8e), 0x0d },
	{ CCI_REG8(0x5ddd), 0x0d },
	{ CCI_REG8(0x5e0e), 0x0d },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x0c },
	{ CCI_REG8(0x0349), 0xcf },
	{ CCI_REG8(0x034a), 0x09 },
	{ CCI_REG8(0x034b), 0x9f },
	{ CCI_REG8(0x034c), 0x06 },
	{ CCI_REG8(0x034d), 0x68 },
	{ CCI_REG8(0x034e), 0x04 },
	{ CCI_REG8(0x034f), 0xd0 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x06 },
	{ CCI_REG8(0x040d), 0x68 },
	{ CCI_REG8(0x040e), 0x04 },
	{ CCI_REG8(0x040f), 0xd0 },
	{ CCI_REG8(0x0301), 0x09 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0xa6 },
	{ CCI_REG8(0x030b), 0x04 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xe6 },
	{ CCI_REG8(0x0310), 0x00 },
	{ CCI_REG8(0x080a), 0x00 },
	{ CCI_REG8(0x080b), 0x5f },
	{ CCI_REG8(0x080c), 0x00 },
	{ CCI_REG8(0x080d), 0x2f },
	{ CCI_REG8(0x080e), 0x00 },
	{ CCI_REG8(0x080f), 0x47 },
	{ CCI_REG8(0x0810), 0x00 },
	{ CCI_REG8(0x0811), 0x3f },
	{ CCI_REG8(0x0812), 0x00 },
	{ CCI_REG8(0x0813), 0x3f },
	{ CCI_REG8(0x0814), 0x00 },
	{ CCI_REG8(0x0815), 0x2f },
	{ CCI_REG8(0x0816), 0x00 },
	{ CCI_REG8(0x0817), 0xaf },
	{ CCI_REG8(0x0818), 0x00 },
	{ CCI_REG8(0x0819), 0x27 },
	{ CCI_REG8(0xe04c), 0x00 },
	{ CCI_REG8(0xe04d), 0x4f },
	{ CCI_REG8(0xe04e), 0x00 },
	{ CCI_REG8(0xe04f), 0x1f },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0x0c },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct cci_reg_sequence imx520_1640x924_30fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0x16 },
	{ CCI_REG8(0x0342), 0x0f },
	{ CCI_REG8(0x0343), 0x20 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x03 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x00 },
	{ CCI_REG8(0x5c8e), 0x0d },
	{ CCI_REG8(0x5ddd), 0x0d },
	{ CCI_REG8(0x5e0e), 0x0d },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0x34 },
	{ CCI_REG8(0x0348), 0x0c },
	{ CCI_REG8(0x0349), 0xcf },
	{ CCI_REG8(0x034a), 0x08 },
	{ CCI_REG8(0x034b), 0x6b },
	{ CCI_REG8(0x034c), 0x06 },
	{ CCI_REG8(0x034d), 0x68 },
	{ CCI_REG8(0x034e), 0x03 },
	{ CCI_REG8(0x034f), 0x9c },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x06 },
	{ CCI_REG8(0x040d), 0x68 },
	{ CCI_REG8(0x040e), 0x03 },
	{ CCI_REG8(0x040f), 0x9c },
	{ CCI_REG8(0x0301), 0x09 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0xa6 },
	{ CCI_REG8(0x030b), 0x04 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xe6 },
	{ CCI_REG8(0x0310), 0x00 },
	{ CCI_REG8(0x080a), 0x00 },
	{ CCI_REG8(0x080b), 0x5f },
	{ CCI_REG8(0x080c), 0x00 },
	{ CCI_REG8(0x080d), 0x2f },
	{ CCI_REG8(0x080e), 0x00 },
	{ CCI_REG8(0x080f), 0x47 },
	{ CCI_REG8(0x0810), 0x00 },
	{ CCI_REG8(0x0811), 0x3f },
	{ CCI_REG8(0x0812), 0x00 },
	{ CCI_REG8(0x0813), 0x3f },
	{ CCI_REG8(0x0814), 0x00 },
	{ CCI_REG8(0x0815), 0x2f },
	{ CCI_REG8(0x0816), 0x00 },
	{ CCI_REG8(0x0817), 0xaf },
	{ CCI_REG8(0x0818), 0x00 },
	{ CCI_REG8(0x0819), 0x27 },
	{ CCI_REG8(0xe04c), 0x00 },
	{ CCI_REG8(0xe04d), 0x4f },
	{ CCI_REG8(0xe04e), 0x00 },
	{ CCI_REG8(0xe04f), 0x1f },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0x0c },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct imx520_mode imx520_supported_modes[] = {
	{
		.width = 3280,
		.height = 2460,
		.hts = 3872,
		.vts = 2742,
		.exposure = 2732,
		.pixel_rate = 318577778,
		.link_freq_index = 0,
		.crop = { 0, 2, 3280, 2460 },
		.reg_list = {
			.regs = imx520_3280x2460_30fps_regs,
			.num_regs = ARRAY_SIZE(imx520_3280x2460_30fps_regs),
		},
	},
	{
		.width = 3280,
		.height = 1848,
		.hts = 3872,
		.vts = 2582,
		.exposure = 2572,
		.pixel_rate = 300088889,
		.link_freq_index = 1,
		.crop = { 0, 308, 3280, 1848 },
		.reg_list = {
			.regs = imx520_3280x1848_30fps_regs,
			.num_regs = ARRAY_SIZE(imx520_3280x1848_30fps_regs),
		},
	},
	{
		.width = 1640,
		.height = 1232,
		.hts = 3872,
		.vts = 2582,
		.exposure = 2572,
		.pixel_rate = 300088889,
		.link_freq_index = 1,
		.crop = { 0, 0, 3280, 2464 },
		.reg_list = {
			.regs = imx520_1640x1232_30fps_regs,
			.num_regs = ARRAY_SIZE(imx520_1640x1232_30fps_regs),
		},
	},
	{
		.width = 1640,
		.height = 924,
		.hts = 3872,
		.vts = 2582,
		.exposure = 2572,
		.pixel_rate = 300088889,
		.link_freq_index = 1,
		.crop = { 0, 308, 3280, 1848 },
		.reg_list = {
			.regs = imx520_1640x924_30fps_regs,
			.num_regs = ARRAY_SIZE(imx520_1640x924_30fps_regs),
		},
	},
};

static int imx520_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx520 *imx520 = container_of(ctrl->handler, struct imx520,
					     ctrl_handler);
	const struct imx520_mode *mode = imx520->mode;
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
		exposure_max = mode->height + ctrl->val - IMX520_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx520->exposure,
					 imx520->exposure->minimum,
					 exposure_max,
					 imx520->exposure->step,
					 imx520->exposure->default_value);
		break;
	}

	/* V4L2 controls are applied, when sensor is powered up for streaming */
	if (!pm_runtime_get_if_active(imx520->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(imx520->regmap, IMX520_REG_DIGITAL_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx520->regmap, IMX520_REG_GROUP_HOLD, 1, NULL);
		cci_write(imx520->regmap, IMX520_REG_EXPOSURE,
			  imx520->exposure->val, &ret);
		cci_write(imx520->regmap, IMX520_REG_ANALOGUE_GAIN,
			  imx520->analogue_gain->val, &ret);
		release = cci_write(imx520->regmap, IMX520_REG_GROUP_HOLD, 0, NULL);
		if (!ret)
			ret = release;
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx520->regmap, IMX520_REG_VTS,
				ctrl->val + mode->height, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx520->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx520_ctrl_ops = {
	.s_ctrl = imx520_set_ctrl,
};

static int imx520_init_controls(struct imx520 *imx520)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx520->ctrl_handler;
	const struct imx520_mode *mode = imx520->mode;
	s64 pixel_rate, hblank, vblank, exposure_max;
	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 9);

	imx520->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL,
						   V4L2_CID_LINK_FREQ,
						  ARRAY_SIZE(imx520_link_freq_menu) - 1,
						  mode->link_freq_index,
						  imx520_link_freq_menu);
	if (imx520->link_freq)
		imx520->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = mode->pixel_rate;
	imx520->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
					       V4L2_CID_PIXEL_RATE,
					       0, pixel_rate, 1, pixel_rate);

	hblank = mode->hts - mode->width;
	imx520->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (imx520->hblank)
		imx520->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	imx520->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   IMX520_VTS_MAX - mode->height, 1,
					   vblank);

	imx520->analogue_gain =
		v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				  IMX520_ANALOGUE_GAIN_MIN, IMX520_ANALOGUE_GAIN_MAX,
				  IMX520_ANALOGUE_GAIN_STEP, IMX520_ANALOGUE_GAIN_DEFAULT);

	imx520->digital_gain = v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops,
						 V4L2_CID_DIGITAL_GAIN,
						 IMX520_DIGITAL_GAIN_MIN,
						 IMX520_DIGITAL_GAIN_MAX,
						 IMX520_DIGITAL_GAIN_STEP,
						 IMX520_DIGITAL_GAIN_DEFAULT);

	exposure_max = mode->vts - IMX520_EXPOSURE_MARGIN;
	imx520->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX520_EXPOSURE_MIN,
					     exposure_max,
					     IMX520_EXPOSURE_STEP,
					     mode->exposure);

	imx520->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx520->hflip)
		imx520->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx520->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx520_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx520->vflip)
		imx520->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(imx520->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx520_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}
	v4l2_ctrl_cluster(2, &imx520->exposure);

	imx520->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int imx520_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx520 *imx520 = to_imx520(sd);
	const struct imx520_reg_list *reg_list = &imx520->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(imx520->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(imx520->regmap, imx520_init_regs,
			    ARRAY_SIZE(imx520_init_regs), &ret);
	cci_multi_reg_write(imx520->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(imx520->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(imx520->regmap, IMX520_REG_ORIENTATION,
		  IMX520_ORIENTATION_FACTORY ^
		  (imx520->hflip->val ? BIT(0) : 0) ^
		  (imx520->vflip->val ? BIT(1) : 0), &ret);
	cci_write(imx520->regmap, IMX520_REG_MODE_SELECT,
		  IMX520_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(imx520->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(imx520->dev);

	return ret;
}

static int imx520_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx520 *imx520 = to_imx520(sd);
	int ret;

	ret = cci_write(imx520->regmap, IMX520_REG_MODE_SELECT,
			IMX520_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx520->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(imx520->dev);

	return ret;
}

static u32 imx520_get_format_code(struct imx520 *imx520)
{
	unsigned int i;

	i = (imx520->vflip->val ? 2 : 0) | (imx520->hflip->val ? 1 : 0);

	return imx520_mbus_formats[i];
}

static void imx520_update_pad_format(struct imx520 *imx520,
				     const struct imx520_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = imx520_get_format_code(imx520);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx520_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx520 *imx520 = to_imx520(sd);
	s64 hblank, vblank, exposure_max;
	const struct imx520_mode *mode;

	mode = v4l2_find_nearest_size(imx520_supported_modes,
				      ARRAY_SIZE(imx520_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	imx520_update_pad_format(imx520, mode, &fmt->format);

	/* Format code could be updated with respect to flip controls */
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || imx520->mode == mode)
		goto set_format;

	imx520->mode = mode;
	__v4l2_ctrl_s_ctrl(imx520->link_freq, mode->link_freq_index);
	__v4l2_ctrl_s_ctrl_int64(imx520->pixel_rate, mode->pixel_rate);

	/* Update limits and set FPS and exposure to default values */
	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(imx520->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(imx520->vblank, vblank,
				 IMX520_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(imx520->vblank, vblank);

	exposure_max = mode->vts - IMX520_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(imx520->exposure, IMX520_EXPOSURE_MIN,
				 exposure_max, IMX520_EXPOSURE_STEP,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(imx520->exposure, mode->exposure);

	if (imx520->sd.ctrl_handler->error)
		return imx520->sd.ctrl_handler->error;

set_format:
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	return 0;
}

static int imx520_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx520 *imx520 = to_imx520(sd);

	/* Media bus code index is constant, but code formats are not */
	if (code->index > 0)
		return -EINVAL;

	code->code = imx520_get_format_code(imx520);

	return 0;
}

static int imx520_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx520 *imx520 = to_imx520(sd);

	if (fse->index >= ARRAY_SIZE(imx520_supported_modes))
		return -EINVAL;

	if (fse->code != imx520_get_format_code(imx520))
		return -EINVAL;

	fse->min_width = imx520_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = imx520_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int imx520_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_config *config)
{
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.num_data_lanes = IMX520_DATA_LANES;

	return 0;
}

static int imx520_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	const struct v4l2_mbus_framefmt *format;
	const struct imx520_mode *mode;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		format = v4l2_subdev_state_get_format(sd_state, 0);
		mode = v4l2_find_nearest_size(imx520_supported_modes,
					      ARRAY_SIZE(imx520_supported_modes),
					      width, height,
					      format->width, format->height);
		sel->r = mode->crop;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = imx520_supported_modes[0].crop;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx520_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx520 *imx520 = to_imx520(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			/* Media bus code depends on current flip controls */
			.width = imx520->mode->width,
			.height = imx520->mode->height,
		},
	};

	imx520_set_pad_format(sd, state, &fmt);

	return 0;
}

static const struct v4l2_subdev_video_ops imx520_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx520_pad_ops = {
	.set_fmt = imx520_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = imx520_get_selection,
	.enum_mbus_code = imx520_enum_mbus_code,
	.enum_frame_size = imx520_enum_frame_size,
	.get_mbus_config = imx520_get_mbus_config,
	.enable_streams = imx520_enable_streams,
	.disable_streams = imx520_disable_streams,
};

static const struct v4l2_subdev_ops imx520_subdev_ops = {
	.video = &imx520_video_ops,
	.pad = &imx520_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx520_internal_ops = {
	.init_state = imx520_init_state,
};

static const struct media_entity_operations imx520_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx520_identify_sensor(struct imx520 *imx520)
{
	u64 val;
	int ret;

	ret = cci_read(imx520->regmap, IMX520_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(imx520->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != IMX520_CHIP_ID) {
		dev_err(imx520->dev, "chip id mismatch: %x!=%llx\n",
			IMX520_CHIP_ID, val);
		return -ENODEV;
	}

	return 0;
}

static int imx520_check_hwcfg(struct imx520 *imx520)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx520->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus = {
			.mipi_csi2 = {
				.num_data_lanes = IMX520_DATA_LANES,
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

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX520_DATA_LANES) {
		dev_err(imx520->dev, "Invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx520->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx520_link_freq_menu,
				       ARRAY_SIZE(imx520_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx520_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx520 *imx520 = to_imx520(sd);
	unsigned int i;
	int ret;

	/* The factory sequence enables VIO, VANA and VDIG in that order. */
	for (i = 0; i < IMX520_NUM_SUPPLIES; i++) {
		ret = regulator_enable(imx520->supplies[i].consumer);
		if (ret)
			goto disable_regulators;
	}
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(imx520->reset_gpio, 0);
	usleep_range(1000, 2000);

	ret = clk_prepare_enable(imx520->mclk);
	if (ret)
		goto assert_reset;
	usleep_range(2000, 3000);

	gpiod_set_value_cansleep(imx520->mclk_request_gpio, 1);
	usleep_range(1000, 2000);

	return 0;

assert_reset:
	gpiod_set_value_cansleep(imx520->reset_gpio, 1);
	usleep_range(1000, 2000);
disable_regulators:
	regulator_bulk_disable(i, imx520->supplies);

	return ret;
}

static int imx520_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx520 *imx520 = to_imx520(sd);
	int ret;

	clk_disable_unprepare(imx520->mclk);
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(imx520->reset_gpio, 1);
	usleep_range(1000, 2000);

	ret = regulator_bulk_disable(IMX520_NUM_SUPPLIES, imx520->supplies);
	gpiod_set_value_cansleep(imx520->mclk_request_gpio, 0);

	return ret;
}

static int imx520_probe(struct i2c_client *client)
{
	struct imx520 *imx520;
	unsigned long freq;
	unsigned int i;
	int ret;

	imx520 = devm_kzalloc(&client->dev, sizeof(*imx520), GFP_KERNEL);
	if (!imx520)
		return -ENOMEM;

	imx520->dev = &client->dev;
	v4l2_i2c_subdev_init(&imx520->sd, client, &imx520_subdev_ops);

	imx520->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx520->regmap))
		return dev_err_probe(imx520->dev, PTR_ERR(imx520->regmap),
				     "failed to init CCI\n");

	imx520->mclk = devm_v4l2_sensor_clk_get(imx520->dev, NULL);
	if (IS_ERR(imx520->mclk))
		return dev_err_probe(imx520->dev, PTR_ERR(imx520->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(imx520->mclk);
	if (freq != IMX520_MCLK_FREQ_19P2MHZ)
		return dev_err_probe(imx520->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = imx520_check_hwcfg(imx520);
	if (ret)
		return dev_err_probe(imx520->dev, ret,
				     "failed to check HW configuration\n");

	imx520->reset_gpio = devm_gpiod_get_optional(imx520->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx520->reset_gpio))
		return dev_err_probe(imx520->dev, PTR_ERR(imx520->reset_gpio),
				     "cannot get reset GPIO\n");

	imx520->mclk_request_gpio =
		devm_gpiod_get_optional(imx520->dev, "mclk-request",
					GPIOD_OUT_LOW);
	if (IS_ERR(imx520->mclk_request_gpio))
		return dev_err_probe(imx520->dev,
				     PTR_ERR(imx520->mclk_request_gpio),
				     "cannot get MCLK request GPIO\n");

	for (i = 0; i < IMX520_NUM_SUPPLIES; i++)
		imx520->supplies[i].supply = imx520_supply_names[i];

	ret = devm_regulator_bulk_get(imx520->dev, IMX520_NUM_SUPPLIES,
				      imx520->supplies);
	if (ret)
		return dev_err_probe(imx520->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = imx520_power_on(imx520->dev);
	if (ret)
		return ret;

	ret = imx520_identify_sensor(imx520);
	if (ret) {
		dev_err_probe(imx520->dev, ret, "failed to find sensor\n");
		goto power_off;
	}

	imx520->mode = &imx520_supported_modes[0];
	ret = imx520_init_controls(imx520);
	if (ret) {
		dev_err_probe(imx520->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	imx520->sd.state_lock = imx520->ctrl_handler.lock;
	imx520->sd.internal_ops = &imx520_internal_ops;
	imx520->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx520->sd.entity.ops = &imx520_subdev_entity_ops;
	imx520->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx520->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx520->sd.entity, 1, &imx520->pad);
	if (ret) {
		dev_err_probe(imx520->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&imx520->sd);
	if (ret < 0) {
		dev_err_probe(imx520->dev, ret,
			      "failed to init media entity pads\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(imx520->dev);
	pm_runtime_enable(imx520->dev);

	ret = v4l2_async_register_subdev_sensor(&imx520->sd);
	if (ret < 0) {
		dev_err_probe(imx520->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(imx520->dev, 1000);
	pm_runtime_use_autosuspend(imx520->dev);
	pm_runtime_idle(imx520->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&imx520->sd);
	pm_runtime_disable(imx520->dev);
	pm_runtime_set_suspended(imx520->dev);

media_entity_cleanup:
	media_entity_cleanup(&imx520->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(imx520->sd.ctrl_handler);

power_off:
	imx520_power_off(imx520->dev);

	return ret;
}

static void imx520_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx520 *imx520 = to_imx520(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(imx520->dev);

	if (!pm_runtime_status_suspended(imx520->dev)) {
		imx520_power_off(imx520->dev);
		pm_runtime_set_suspended(imx520->dev);
	}
}

static const struct dev_pm_ops imx520_pm_ops = {
	SET_RUNTIME_PM_OPS(imx520_power_off, imx520_power_on, NULL)
};

static const struct of_device_id imx520_of_match[] = {
	{ .compatible = "sony,imx520" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx520_of_match);

static struct i2c_driver imx520_i2c_driver = {
	.driver = {
		.name = "imx520",
		.pm = &imx520_pm_ops,
		.of_match_table = imx520_of_match,
	},
	.probe = imx520_probe,
	.remove = imx520_remove,
};

module_i2c_driver(imx520_i2c_driver);

MODULE_DESCRIPTION("Sony IMX520 image sensor driver");
MODULE_LICENSE("GPL");

