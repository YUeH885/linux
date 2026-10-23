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

#define IMX351_LINK_FREQ_664P8MHZ	664800000ULL
#define IMX351_LINK_FREQ_510MHZ		510000000ULL
#define IMX351_MCLK_FREQ_19P2MHZ		(19200 * HZ_PER_KHZ)
#define IMX351_DATA_LANES		4

#define IMX351_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX351_MODE_STANDBY		0x00
#define IMX351_MODE_STREAMING		0x01

#define IMX351_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX351_ORIENTATION_FACTORY	0x03

#define IMX351_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX351_CHIP_ID			0x0351

#define IMX351_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX351_EXPOSURE_MIN		8
#define IMX351_EXPOSURE_STEP		1
#define IMX351_EXPOSURE_MARGIN		20

#define IMX351_REG_ANALOGUE_GAIN	CCI_REG16(0x0204)
#define IMX351_ANALOGUE_GAIN_MIN	0
#define IMX351_ANALOGUE_GAIN_MAX	960
#define IMX351_ANALOGUE_GAIN_STEP	1
#define IMX351_ANALOGUE_GAIN_DEFAULT	0

#define IMX351_REG_DIGITAL_GAIN		CCI_REG16(0x020e)
#define IMX351_DIGITAL_GAIN_MIN		256
#define IMX351_DIGITAL_GAIN_MAX		4095
#define IMX351_DIGITAL_GAIN_STEP	1
#define IMX351_DIGITAL_GAIN_DEFAULT	256

#define IMX351_REG_VTS			CCI_REG16(0x0340)
#define IMX351_VTS_MAX			0xffff

#define to_imx351(_sd)			container_of(_sd, struct imx351, sd)

static const s64 imx351_link_freq_menu[] = {
	IMX351_LINK_FREQ_664P8MHZ,
	IMX351_LINK_FREQ_510MHZ,
};

/* The factory readout has both orientation bits enabled. */
static const u32 imx351_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
};

struct imx351_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct imx351_mode {
	u32 width;			/* Frame width in pixels */
	u32 height;			/* Frame height in pixels */
	u32 hts;			/* Horizontal timing size */
	u32 vts;			/* Default vertical timing size */
	u32 exposure;			/* Default exposure value */
	u64 pixel_rate;
	u8 link_freq_index;
	struct v4l2_rect crop;

	const struct imx351_reg_list reg_list;	/* Sensor register setting */
};

static const char * const imx351_supply_names[] = {
	"vdda",		/* Analogue power */
	"vddd",		/* Digital core power */
	"vddio",	/* Digital I/O power */
};

#define IMX351_NUM_SUPPLIES	ARRAY_SIZE(imx351_supply_names)

struct imx351 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX351_NUM_SUPPLIES];

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *digital_gain;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	const struct imx351_mode *mode;
};

static const struct cci_reg_sequence imx351_init_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x3c7d), 0x28 },
	{ CCI_REG8(0x3c7e), 0x01 },
	{ CCI_REG8(0x3c7f), 0x0f },
	{ CCI_REG8(0x3140), 0x02 },
	{ CCI_REG8(0x3f7f), 0x01 },
	{ CCI_REG8(0x4430), 0x05 },
	{ CCI_REG8(0x4431), 0xdc },
	{ CCI_REG8(0x4ed0), 0x01 },
	{ CCI_REG8(0x4ed1), 0x3e },
	{ CCI_REG8(0x4ede), 0x01 },
	{ CCI_REG8(0x4edf), 0x45 },
	{ CCI_REG8(0x5222), 0x02 },
	{ CCI_REG8(0x5617), 0x0a },
	{ CCI_REG8(0x562b), 0x0a },
	{ CCI_REG8(0x562d), 0x0c },
	{ CCI_REG8(0x56b7), 0x74 },
	{ CCI_REG8(0x6282), 0x82 },
	{ CCI_REG8(0x6283), 0x80 },
	{ CCI_REG8(0x6286), 0x07 },
	{ CCI_REG8(0x6287), 0xc0 },
	{ CCI_REG8(0x6288), 0x08 },
	{ CCI_REG8(0x628a), 0x18 },
	{ CCI_REG8(0x628b), 0x80 },
	{ CCI_REG8(0x628c), 0x20 },
	{ CCI_REG8(0x628e), 0x32 },
	{ CCI_REG8(0x6290), 0x40 },
	{ CCI_REG8(0x6292), 0x0a },
	{ CCI_REG8(0x6296), 0x50 },
	{ CCI_REG8(0x629a), 0xf8 },
	{ CCI_REG8(0x629b), 0x01 },
	{ CCI_REG8(0x629d), 0x03 },
	{ CCI_REG8(0x629f), 0x04 },
	{ CCI_REG8(0x62b1), 0x06 },
	{ CCI_REG8(0x62b5), 0x3c },
	{ CCI_REG8(0x62b9), 0xc8 },
	{ CCI_REG8(0x62bc), 0x02 },
	{ CCI_REG8(0x62bd), 0x70 },
	{ CCI_REG8(0x62d0), 0x06 },
	{ CCI_REG8(0x62d4), 0x38 },
	{ CCI_REG8(0x62d8), 0xb8 },
	{ CCI_REG8(0x62db), 0x02 },
	{ CCI_REG8(0x62dc), 0x40 },
	{ CCI_REG8(0x62dd), 0x03 },
	{ CCI_REG8(0x637a), 0x11 },
	{ CCI_REG8(0x7ba0), 0x01 },
	{ CCI_REG8(0x7ba9), 0x00 },
	{ CCI_REG8(0x7baa), 0x01 },
	{ CCI_REG8(0x7bad), 0x00 },
	{ CCI_REG8(0x886b), 0x00 },
	{ CCI_REG8(0x9002), 0x00 },
	{ CCI_REG8(0x9003), 0x00 },
	{ CCI_REG8(0x9004), 0x09 },
	{ CCI_REG8(0x9006), 0x01 },
	{ CCI_REG8(0x9200), 0x93 },
	{ CCI_REG8(0x9201), 0x85 },
	{ CCI_REG8(0x9202), 0x93 },
	{ CCI_REG8(0x9203), 0x87 },
	{ CCI_REG8(0x9204), 0x93 },
	{ CCI_REG8(0x9205), 0x8d },
	{ CCI_REG8(0x9206), 0x93 },
	{ CCI_REG8(0x9207), 0x8f },
	{ CCI_REG8(0x9208), 0x6a },
	{ CCI_REG8(0x9209), 0x22 },
	{ CCI_REG8(0x920a), 0x6a },
	{ CCI_REG8(0x920b), 0x23 },
	{ CCI_REG8(0x920c), 0x6a },
	{ CCI_REG8(0x920d), 0x0f },
	{ CCI_REG8(0x920e), 0x71 },
	{ CCI_REG8(0x920f), 0x03 },
	{ CCI_REG8(0x9210), 0x71 },
	{ CCI_REG8(0x9211), 0x0b },
	{ CCI_REG8(0x935d), 0x01 },
	{ CCI_REG8(0x9389), 0x05 },
	{ CCI_REG8(0x938b), 0x05 },
	{ CCI_REG8(0x9391), 0x05 },
	{ CCI_REG8(0x9393), 0x05 },
	{ CCI_REG8(0x9395), 0x82 },
	{ CCI_REG8(0x9397), 0x78 },
	{ CCI_REG8(0x9399), 0x05 },
	{ CCI_REG8(0x939b), 0x05 },
	{ CCI_REG8(0xa91f), 0x04 },
	{ CCI_REG8(0xa921), 0x03 },
	{ CCI_REG8(0xa923), 0x02 },
	{ CCI_REG8(0xa93d), 0x05 },
	{ CCI_REG8(0xa93f), 0x03 },
	{ CCI_REG8(0xa941), 0x02 },
	{ CCI_REG8(0xa9af), 0x04 },
	{ CCI_REG8(0xa9b1), 0x03 },
	{ CCI_REG8(0xa9b3), 0x02 },
	{ CCI_REG8(0xa9cd), 0x05 },
	{ CCI_REG8(0xa9cf), 0x03 },
	{ CCI_REG8(0xa9d1), 0x02 },
	{ CCI_REG8(0xaa3f), 0x04 },
	{ CCI_REG8(0xaa41), 0x03 },
	{ CCI_REG8(0xaa43), 0x02 },
	{ CCI_REG8(0xaa5d), 0x05 },
	{ CCI_REG8(0xaa5f), 0x03 },
	{ CCI_REG8(0xaa61), 0x02 },
	{ CCI_REG8(0xaacf), 0x04 },
	{ CCI_REG8(0xaad1), 0x03 },
	{ CCI_REG8(0xaad3), 0x02 },
	{ CCI_REG8(0xaaed), 0x05 },
	{ CCI_REG8(0xaaef), 0x03 },
	{ CCI_REG8(0xaaf1), 0x02 },
	{ CCI_REG8(0xab87), 0x04 },
	{ CCI_REG8(0xab89), 0x03 },
	{ CCI_REG8(0xab8b), 0x02 },
	{ CCI_REG8(0xaba5), 0x05 },
	{ CCI_REG8(0xaba7), 0x03 },
	{ CCI_REG8(0xaba9), 0x02 },
	{ CCI_REG8(0xabb7), 0x04 },
	{ CCI_REG8(0xabb9), 0x03 },
	{ CCI_REG8(0xabbb), 0x02 },
	{ CCI_REG8(0xabd5), 0x05 },
	{ CCI_REG8(0xabd7), 0x03 },
	{ CCI_REG8(0xabd9), 0x02 },
	{ CCI_REG8(0xb388), 0x28 },
	{ CCI_REG8(0xbc40), 0x03 },
	{ CCI_REG8(0x7b80), 0x00 },
	{ CCI_REG8(0x7b81), 0x00 },
	{ CCI_REG8(0x8d1f), 0x00 },
	{ CCI_REG8(0x8d27), 0x00 },
	{ CCI_REG8(0x9963), 0x64 },
	{ CCI_REG8(0x9964), 0x50 },
	{ CCI_REG8(0x9a00), 0x0c },
	{ CCI_REG8(0x9a01), 0x0c },
	{ CCI_REG8(0x9a06), 0x0c },
	{ CCI_REG8(0x9a18), 0x0c },
	{ CCI_REG8(0x9a19), 0x0c },
	{ CCI_REG8(0xa900), 0x20 },
	{ CCI_REG8(0xa901), 0x20 },
	{ CCI_REG8(0xa902), 0x20 },
	{ CCI_REG8(0xa903), 0x15 },
	{ CCI_REG8(0xa904), 0x15 },
	{ CCI_REG8(0xa905), 0x15 },
	{ CCI_REG8(0xa906), 0x20 },
	{ CCI_REG8(0xa907), 0x20 },
	{ CCI_REG8(0xa908), 0x20 },
	{ CCI_REG8(0xa909), 0x15 },
	{ CCI_REG8(0xa90a), 0x15 },
	{ CCI_REG8(0xa90b), 0x15 },
	{ CCI_REG8(0xa915), 0x3f },
	{ CCI_REG8(0xa916), 0x3f },
	{ CCI_REG8(0xa917), 0x3f },
	{ CCI_REG8(0xa949), 0x03 },
	{ CCI_REG8(0xa94b), 0x03 },
	{ CCI_REG8(0xa94d), 0x03 },
	{ CCI_REG8(0xa94f), 0x06 },
	{ CCI_REG8(0xa951), 0x06 },
	{ CCI_REG8(0xa953), 0x06 },
	{ CCI_REG8(0xa955), 0x03 },
	{ CCI_REG8(0xa957), 0x03 },
	{ CCI_REG8(0xa959), 0x03 },
	{ CCI_REG8(0xa95b), 0x06 },
	{ CCI_REG8(0xa95d), 0x06 },
	{ CCI_REG8(0xa95f), 0x06 },
	{ CCI_REG8(0xa98b), 0x1f },
	{ CCI_REG8(0xa98d), 0x1f },
	{ CCI_REG8(0xa98f), 0x1f },
	{ CCI_REG8(0xaa20), 0x3f },
	{ CCI_REG8(0xaa21), 0x20 },
	{ CCI_REG8(0xaa22), 0x20 },
	{ CCI_REG8(0xaa23), 0x3f },
	{ CCI_REG8(0xaa24), 0x15 },
	{ CCI_REG8(0xaa25), 0x15 },
	{ CCI_REG8(0xaa26), 0x20 },
	{ CCI_REG8(0xaa27), 0x20 },
	{ CCI_REG8(0xaa28), 0x20 },
	{ CCI_REG8(0xaa29), 0x15 },
	{ CCI_REG8(0xaa2a), 0x15 },
	{ CCI_REG8(0xaa2b), 0x15 },
	{ CCI_REG8(0xaa32), 0x3f },
	{ CCI_REG8(0xaa35), 0x3f },
	{ CCI_REG8(0xaa36), 0x3f },
	{ CCI_REG8(0xaa37), 0x3f },
	{ CCI_REG8(0xaa69), 0x3f },
	{ CCI_REG8(0xaa6b), 0x03 },
	{ CCI_REG8(0xaa6d), 0x03 },
	{ CCI_REG8(0xaa6f), 0x3f },
	{ CCI_REG8(0xaa71), 0x06 },
	{ CCI_REG8(0xaa73), 0x06 },
	{ CCI_REG8(0xaa75), 0x03 },
	{ CCI_REG8(0xaa77), 0x03 },
	{ CCI_REG8(0xaa79), 0x03 },
	{ CCI_REG8(0xaa7b), 0x06 },
	{ CCI_REG8(0xaa7d), 0x06 },
	{ CCI_REG8(0xaa7f), 0x06 },
	{ CCI_REG8(0xaaab), 0x1f },
	{ CCI_REG8(0xaaad), 0x1f },
	{ CCI_REG8(0xaaaf), 0x1f },
	{ CCI_REG8(0xaab0), 0x20 },
	{ CCI_REG8(0xaab1), 0x20 },
	{ CCI_REG8(0xaab2), 0x20 },
	{ CCI_REG8(0xaac2), 0x3f },
	{ CCI_REG8(0xab53), 0x20 },
	{ CCI_REG8(0xab54), 0x20 },
	{ CCI_REG8(0xab55), 0x20 },
	{ CCI_REG8(0xab57), 0x40 },
	{ CCI_REG8(0xab59), 0x40 },
	{ CCI_REG8(0xab5b), 0x40 },
	{ CCI_REG8(0xab63), 0x03 },
	{ CCI_REG8(0xab65), 0x03 },
	{ CCI_REG8(0xab67), 0x03 },
	{ CCI_REG8(0xac01), 0x0a },
	{ CCI_REG8(0xac03), 0x0a },
	{ CCI_REG8(0xac05), 0x0a },
	{ CCI_REG8(0xac06), 0x01 },
	{ CCI_REG8(0xac07), 0xc0 },
	{ CCI_REG8(0xac09), 0xc0 },
	{ CCI_REG8(0xac17), 0x0a },
	{ CCI_REG8(0xac19), 0x0a },
	{ CCI_REG8(0xac1b), 0x0a },
	{ CCI_REG8(0xac1c), 0x01 },
	{ CCI_REG8(0xac1d), 0xc0 },
	{ CCI_REG8(0xac1f), 0xc0 },
	{ CCI_REG8(0x0101), 0x03 },
};

static const struct cci_reg_sequence imx351_4656x3492_30fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x17 },
	{ CCI_REG8(0x0343), 0x90 },
	{ CCI_REG8(0x0340), 0x0e },
	{ CCI_REG8(0x0341), 0x14 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x12 },
	{ CCI_REG8(0x0349), 0x2f },
	{ CCI_REG8(0x034a), 0x0d },
	{ CCI_REG8(0x034b), 0xa3 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x0a },
	{ CCI_REG8(0x3243), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x01 },
	{ CCI_REG8(0x4254), 0x7f },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x12 },
	{ CCI_REG8(0x040d), 0x30 },
	{ CCI_REG8(0x040e), 0x0d },
	{ CCI_REG8(0x040f), 0xa4 },
	{ CCI_REG8(0x034c), 0x12 },
	{ CCI_REG8(0x034d), 0x30 },
	{ CCI_REG8(0x034e), 0x0d },
	{ CCI_REG8(0x034f), 0xa4 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x02 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xaa },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0x15 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0820), 0x14 },
	{ CCI_REG8(0x0821), 0xc6 },
	{ CCI_REG8(0x0822), 0x66 },
	{ CCI_REG8(0x0823), 0x66 },
	{ CCI_REG8(0xbc41), 0x01 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x00 },
	{ CCI_REG8(0x3e3b), 0x00 },
	{ CCI_REG8(0x0106), 0x00 },
	{ CCI_REG8(0x0b00), 0x00 },
	{ CCI_REG8(0x3230), 0x00 },
	{ CCI_REG8(0x3c00), 0x5b },
	{ CCI_REG8(0x3c01), 0x54 },
	{ CCI_REG8(0x3c02), 0x77 },
	{ CCI_REG8(0x3c03), 0x66 },
	{ CCI_REG8(0x3c04), 0x00 },
	{ CCI_REG8(0x3c05), 0xc8 },
	{ CCI_REG8(0x3c06), 0x14 },
	{ CCI_REG8(0x3c07), 0x00 },
	{ CCI_REG8(0x3c08), 0x01 },
	{ CCI_REG8(0x3f14), 0x01 },
	{ CCI_REG8(0x3f17), 0x00 },
	{ CCI_REG8(0x3f3c), 0x01 },
	{ CCI_REG8(0x3f78), 0x00 },
	{ CCI_REG8(0x3f79), 0xb4 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x00 },
	{ CCI_REG8(0x97c1), 0x00 },
	{ CCI_REG8(0x97c5), 0x14 },
	{ CCI_REG8(0x620d), 0xd0 },
	{ CCI_REG8(0x620e), 0x27 },
	{ CCI_REG8(0x6399), 0xf4 },
	{ CCI_REG8(0x6f6e), 0x02 },
	{ CCI_REG8(0x0202), 0x0e },
	{ CCI_REG8(0x0203), 0x00 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct cci_reg_sequence imx351_2328x1312_60fps_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x0c },
	{ CCI_REG8(0x0343), 0x94 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0x78 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0xb0 },
	{ CCI_REG8(0x0348), 0x12 },
	{ CCI_REG8(0x0349), 0x2f },
	{ CCI_REG8(0x034a), 0x0b },
	{ CCI_REG8(0x034b), 0xef },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x0a },
	{ CCI_REG8(0x3243), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x01 },
	{ CCI_REG8(0x4254), 0x7f },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x09 },
	{ CCI_REG8(0x040d), 0x18 },
	{ CCI_REG8(0x040e), 0x05 },
	{ CCI_REG8(0x040f), 0x20 },
	{ CCI_REG8(0x034c), 0x09 },
	{ CCI_REG8(0x034d), 0x18 },
	{ CCI_REG8(0x034e), 0x05 },
	{ CCI_REG8(0x034f), 0x20 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x02 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x87 },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0xa9 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0820), 0x0f },
	{ CCI_REG8(0x0821), 0xf0 },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0xbc41), 0x01 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x00 },
	{ CCI_REG8(0x3e3b), 0x00 },
	{ CCI_REG8(0x0106), 0x00 },
	{ CCI_REG8(0x0b00), 0x00 },
	{ CCI_REG8(0x3230), 0x00 },
	{ CCI_REG8(0x3c00), 0x5b },
	{ CCI_REG8(0x3c01), 0x54 },
	{ CCI_REG8(0x3c02), 0x77 },
	{ CCI_REG8(0x3c03), 0x66 },
	{ CCI_REG8(0x3c04), 0x00 },
	{ CCI_REG8(0x3c05), 0xc4 },
	{ CCI_REG8(0x3c06), 0x14 },
	{ CCI_REG8(0x3c07), 0x00 },
	{ CCI_REG8(0x3c08), 0x01 },
	{ CCI_REG8(0x3f14), 0x01 },
	{ CCI_REG8(0x3f17), 0x00 },
	{ CCI_REG8(0x3f3c), 0x01 },
	{ CCI_REG8(0x3f78), 0x02 },
	{ CCI_REG8(0x3f79), 0xcc },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x00 },
	{ CCI_REG8(0x97c1), 0x04 },
	{ CCI_REG8(0x97c5), 0x0c },
	{ CCI_REG8(0x620d), 0xd0 },
	{ CCI_REG8(0x620e), 0x27 },
	{ CCI_REG8(0x6399), 0xf4 },
	{ CCI_REG8(0x6f6e), 0x02 },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0x64 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct imx351_mode imx351_supported_modes[] = {
	{
		.width = 4656,
		.height = 3492,
		.hts = 6032,
		.vts = 3604,
		.exposure = 3584,
		.pixel_rate = 531840000,
		.link_freq_index = 0,
		.crop = { 0, 0, 4656, 3492 },
		.reg_list = {
			.regs = imx351_4656x3492_30fps_regs,
			.num_regs = ARRAY_SIZE(imx351_4656x3492_30fps_regs),
		},
	},
	{
		.width = 2328,
		.height = 1312,
		.hts = 3220,
		.vts = 2680,
		.exposure = 2660,
		.pixel_rate = 408000000,
		.link_freq_index = 1,
		.crop = { 0, 432, 4656, 2624 },
		.reg_list = {
			.regs = imx351_2328x1312_60fps_regs,
			.num_regs = ARRAY_SIZE(imx351_2328x1312_60fps_regs),
		},
	},
};

static int imx351_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx351 *imx351 = container_of(ctrl->handler, struct imx351,
					     ctrl_handler);
	const struct imx351_mode *mode = imx351->mode;
	s64 exposure_max;
	int ret;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/* The orientation settings are applied along with streaming */
		return 0;
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		exposure_max = mode->height + ctrl->val - IMX351_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx351->exposure,
					 imx351->exposure->minimum,
					 exposure_max,
					 imx351->exposure->step,
					 imx351->exposure->default_value);
		break;
	}

	/* V4L2 controls are applied, when sensor is powered up for streaming */
	if (!pm_runtime_get_if_active(imx351->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx351->regmap, IMX351_REG_ANALOGUE_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(imx351->regmap, IMX351_REG_DIGITAL_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx351->regmap, IMX351_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx351->regmap, IMX351_REG_VTS,
				ctrl->val + mode->height, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx351->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx351_ctrl_ops = {
	.s_ctrl = imx351_set_ctrl,
};

static int imx351_init_controls(struct imx351 *imx351)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx351->ctrl_handler;
	const struct imx351_mode *mode = imx351->mode;
	s64 pixel_rate, hblank, vblank, exposure_max;
	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 9);

	imx351->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL,
						   V4L2_CID_LINK_FREQ,
						  ARRAY_SIZE(imx351_link_freq_menu) - 1,
						  mode->link_freq_index,
						  imx351_link_freq_menu);
	if (imx351->link_freq)
		imx351->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = mode->pixel_rate;
	imx351->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
					       V4L2_CID_PIXEL_RATE,
					       0, pixel_rate, 1, pixel_rate);

	hblank = mode->hts - mode->width;
	imx351->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (imx351->hblank)
		imx351->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	imx351->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   IMX351_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX351_ANALOGUE_GAIN_MIN, IMX351_ANALOGUE_GAIN_MAX,
			  IMX351_ANALOGUE_GAIN_STEP, IMX351_ANALOGUE_GAIN_DEFAULT);

	imx351->digital_gain = v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops,
						 V4L2_CID_DIGITAL_GAIN,
						 IMX351_DIGITAL_GAIN_MIN,
						 IMX351_DIGITAL_GAIN_MAX,
						 IMX351_DIGITAL_GAIN_STEP,
						 IMX351_DIGITAL_GAIN_DEFAULT);

	exposure_max = mode->vts - IMX351_EXPOSURE_MARGIN;
	imx351->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX351_EXPOSURE_MIN,
					     exposure_max,
					     IMX351_EXPOSURE_STEP,
					     mode->exposure);

	imx351->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx351->hflip)
		imx351->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx351->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx351_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx351->vflip)
		imx351->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(imx351->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx351_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	imx351->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int imx351_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx351 *imx351 = to_imx351(sd);
	const struct imx351_reg_list *reg_list = &imx351->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(imx351->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(imx351->regmap, imx351_init_regs,
			    ARRAY_SIZE(imx351_init_regs), &ret);
	cci_multi_reg_write(imx351->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(imx351->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(imx351->regmap, IMX351_REG_ORIENTATION,
		  IMX351_ORIENTATION_FACTORY ^
		  (imx351->hflip->val ? BIT(0) : 0) ^
		  (imx351->vflip->val ? BIT(1) : 0), &ret);
	cci_write(imx351->regmap, IMX351_REG_MODE_SELECT,
		  IMX351_MODE_STREAMING, &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(imx351->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(imx351->dev);

	return ret;
}

static int imx351_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx351 *imx351 = to_imx351(sd);
	int ret;

	ret = cci_write(imx351->regmap, IMX351_REG_MODE_SELECT,
			IMX351_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx351->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(imx351->dev);

	return ret;
}

static u32 imx351_get_format_code(struct imx351 *imx351)
{
	unsigned int i;

	i = (imx351->vflip->val ? 2 : 0) | (imx351->hflip->val ? 1 : 0);

	return imx351_mbus_formats[i];
}

static void imx351_update_pad_format(struct imx351 *imx351,
				     const struct imx351_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = imx351_get_format_code(imx351);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx351_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx351 *imx351 = to_imx351(sd);
	s64 hblank, vblank, exposure_max;
	const struct imx351_mode *mode;

	mode = v4l2_find_nearest_size(imx351_supported_modes,
				      ARRAY_SIZE(imx351_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	imx351_update_pad_format(imx351, mode, &fmt->format);

	/* Format code could be updated with respect to flip controls */
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || imx351->mode == mode)
		goto set_format;

	imx351->mode = mode;
	__v4l2_ctrl_s_ctrl(imx351->link_freq, mode->link_freq_index);
	__v4l2_ctrl_s_ctrl_int64(imx351->pixel_rate, mode->pixel_rate);

	/* Update limits and set FPS and exposure to default values */
	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(imx351->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(imx351->vblank, vblank,
				 IMX351_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(imx351->vblank, vblank);

	exposure_max = mode->vts - IMX351_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(imx351->exposure, IMX351_EXPOSURE_MIN,
				 exposure_max, IMX351_EXPOSURE_STEP,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(imx351->exposure, mode->exposure);

	if (imx351->sd.ctrl_handler->error)
		return imx351->sd.ctrl_handler->error;

set_format:
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	return 0;
}

static int imx351_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx351 *imx351 = to_imx351(sd);

	/* Media bus code index is constant, but code formats are not */
	if (code->index > 0)
		return -EINVAL;

	code->code = imx351_get_format_code(imx351);

	return 0;
}

static int imx351_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx351 *imx351 = to_imx351(sd);

	if (fse->index >= ARRAY_SIZE(imx351_supported_modes))
		return -EINVAL;

	if (fse->code != imx351_get_format_code(imx351))
		return -EINVAL;

	fse->min_width = imx351_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = imx351_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int imx351_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	const struct v4l2_mbus_framefmt *format;
	const struct imx351_mode *mode;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		format = v4l2_subdev_state_get_format(sd_state, 0);
		mode = v4l2_find_nearest_size(imx351_supported_modes,
					      ARRAY_SIZE(imx351_supported_modes),
					      width, height,
					      format->width, format->height);
		sel->r = mode->crop;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = imx351_supported_modes[0].crop;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx351_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx351 *imx351 = to_imx351(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			/* Media bus code depends on current flip controls */
			.width = imx351->mode->width,
			.height = imx351->mode->height,
		},
	};

	imx351_set_pad_format(sd, state, &fmt);

	return 0;
}

static const struct v4l2_subdev_video_ops imx351_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx351_pad_ops = {
	.set_fmt = imx351_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = imx351_get_selection,
	.enum_mbus_code = imx351_enum_mbus_code,
	.enum_frame_size = imx351_enum_frame_size,
	.enable_streams = imx351_enable_streams,
	.disable_streams = imx351_disable_streams,
};

static const struct v4l2_subdev_ops imx351_subdev_ops = {
	.video = &imx351_video_ops,
	.pad = &imx351_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx351_internal_ops = {
	.init_state = imx351_init_state,
};

static const struct media_entity_operations imx351_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx351_identify_sensor(struct imx351 *imx351)
{
	u64 val;
	int ret;

	ret = cci_read(imx351->regmap, IMX351_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(imx351->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != IMX351_CHIP_ID) {
		dev_err(imx351->dev, "chip id mismatch: %x!=%llx\n",
			IMX351_CHIP_ID, val);
		return -ENODEV;
	}

	return 0;
}

static int imx351_check_hwcfg(struct imx351 *imx351)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx351->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus = {
			.mipi_csi2 = {
				.num_data_lanes = IMX351_DATA_LANES,
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

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX351_DATA_LANES) {
		dev_err(imx351->dev, "Invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx351->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx351_link_freq_menu,
				       ARRAY_SIZE(imx351_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx351_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx351 *imx351 = to_imx351(sd);
	int ret;

	ret = regulator_bulk_enable(IMX351_NUM_SUPPLIES, imx351->supplies);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = clk_prepare_enable(imx351->mclk);
	if (ret)
		goto disable_regulators;
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(imx351->reset_gpio, 0);
	usleep_range(1000, 2000);

	return 0;

disable_regulators:
	regulator_bulk_disable(IMX351_NUM_SUPPLIES, imx351->supplies);

	return ret;
}

static int imx351_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx351 *imx351 = to_imx351(sd);
	int ret;

	gpiod_set_value_cansleep(imx351->reset_gpio, 1);
	usleep_range(1000, 2000);

	clk_disable_unprepare(imx351->mclk);
	usleep_range(1000, 2000);

	ret = regulator_bulk_disable(IMX351_NUM_SUPPLIES, imx351->supplies);

	return ret;
}

static int imx351_probe(struct i2c_client *client)
{
	struct imx351 *imx351;
	unsigned long freq;
	unsigned int i;
	int ret;

	imx351 = devm_kzalloc(&client->dev, sizeof(*imx351), GFP_KERNEL);
	if (!imx351)
		return -ENOMEM;

	imx351->dev = &client->dev;
	v4l2_i2c_subdev_init(&imx351->sd, client, &imx351_subdev_ops);

	imx351->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx351->regmap))
		return dev_err_probe(imx351->dev, PTR_ERR(imx351->regmap),
				     "failed to init CCI\n");

	imx351->mclk = devm_v4l2_sensor_clk_get(imx351->dev, NULL);
	if (IS_ERR(imx351->mclk))
		return dev_err_probe(imx351->dev, PTR_ERR(imx351->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(imx351->mclk);
	if (freq != IMX351_MCLK_FREQ_19P2MHZ)
		return dev_err_probe(imx351->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = imx351_check_hwcfg(imx351);
	if (ret)
		return dev_err_probe(imx351->dev, ret,
				     "failed to check HW configuration\n");

	imx351->reset_gpio = devm_gpiod_get_optional(imx351->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx351->reset_gpio))
		return dev_err_probe(imx351->dev, PTR_ERR(imx351->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < IMX351_NUM_SUPPLIES; i++)
		imx351->supplies[i].supply = imx351_supply_names[i];

	ret = devm_regulator_bulk_get(imx351->dev, IMX351_NUM_SUPPLIES,
				      imx351->supplies);
	if (ret)
		return dev_err_probe(imx351->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = imx351_power_on(imx351->dev);
	if (ret)
		return ret;

	ret = imx351_identify_sensor(imx351);
	if (ret) {
		dev_err_probe(imx351->dev, ret, "failed to find sensor\n");
		goto power_off;
	}

	imx351->mode = &imx351_supported_modes[0];
	ret = imx351_init_controls(imx351);
	if (ret) {
		dev_err_probe(imx351->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	imx351->sd.state_lock = imx351->ctrl_handler.lock;
	imx351->sd.internal_ops = &imx351_internal_ops;
	imx351->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx351->sd.entity.ops = &imx351_subdev_entity_ops;
	imx351->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx351->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx351->sd.entity, 1, &imx351->pad);
	if (ret) {
		dev_err_probe(imx351->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&imx351->sd);
	if (ret < 0) {
		dev_err_probe(imx351->dev, ret,
			      "failed to init media entity pads\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(imx351->dev);
	pm_runtime_enable(imx351->dev);

	ret = v4l2_async_register_subdev_sensor(&imx351->sd);
	if (ret < 0) {
		dev_err_probe(imx351->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(imx351->dev, 1000);
	pm_runtime_use_autosuspend(imx351->dev);
	pm_runtime_idle(imx351->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&imx351->sd);
	pm_runtime_disable(imx351->dev);
	pm_runtime_set_suspended(imx351->dev);

media_entity_cleanup:
	media_entity_cleanup(&imx351->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(imx351->sd.ctrl_handler);

power_off:
	imx351_power_off(imx351->dev);

	return ret;
}

static void imx351_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx351 *imx351 = to_imx351(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(imx351->dev);

	if (!pm_runtime_status_suspended(imx351->dev)) {
		imx351_power_off(imx351->dev);
		pm_runtime_set_suspended(imx351->dev);
	}
}

static const struct dev_pm_ops imx351_pm_ops = {
	SET_RUNTIME_PM_OPS(imx351_power_off, imx351_power_on, NULL)
};

static const struct of_device_id imx351_of_match[] = {
	{ .compatible = "sony,imx351" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx351_of_match);

static struct i2c_driver imx351_i2c_driver = {
	.driver = {
		.name = "imx351",
		.pm = &imx351_pm_ops,
		.of_match_table = imx351_of_match,
	},
	.probe = imx351_probe,
	.remove = imx351_remove,
};

module_i2c_driver(imx351_i2c_driver);

MODULE_DESCRIPTION("Sony IMX351 image sensor driver");
MODULE_LICENSE("GPL");
