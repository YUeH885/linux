// SPDX-License-Identifier: GPL-2.0-only
/*
 * LM-G820N 使用的 LG SW43410 MIPI DSI 面板。
 *
 * 初始化表取自原厂固件使用的 SW43410 MP 面板定义。
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

static const struct regulator_bulk_data sw43410_supplies[] = {
	{ .supply = "vddi", .init_load_uA = 62000 },
	{ .supply = "vpnl", .init_load_uA = 857000 },
};

struct sw43410_panel {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct drm_dsc_config dsc;
};

static inline struct sw43410_panel *to_sw43410(struct drm_panel *panel)
{
	return container_of(panel, struct sw43410_panel, base);
}

static const u8 sw43410_dsc_config[] = {
	0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0c,
	0x30, 0x05, 0xa0, 0x00, 0x10, 0x02, 0xd0, 0x02,
	0xd0, 0x02, 0x00, 0x02, 0x68, 0x00, 0x20, 0x01,
	0xbb, 0x00, 0x0a, 0x00, 0x0c, 0x06, 0x67, 0x04,
	0xc5, 0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20,
	0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a,
	0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79,
	0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09,
	0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19,
	0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a,
	0xf6, 0x2b, 0x34, 0x2b, 0x74, 0x3b, 0x74, 0x6b,
	0xf4,
};

static void sw43410_write_dsc_config(struct mipi_dsi_multi_context *dsi_ctx,
				     u8 command)
{
	u8 data[1 + ARRAY_SIZE(sw43410_dsc_config)];

	data[0] = command;
	memcpy(&data[1], sw43410_dsc_config, ARRAY_SIZE(sw43410_dsc_config));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, data, ARRAY_SIZE(data));
}

static const u8 sw43410_cc[] = {
	0xcc, 0x88, 0x0a, 0x4b, 0x6c, 0xff, 0x58, 0x60,
	0x60, 0x80, 0x60, 0x60, 0x80, 0x7a, 0x74, 0x6e,
	0x60, 0x79, 0x65, 0x60, 0x60, 0x68, 0x70, 0x58,
	0x87, 0x7a, 0x74, 0x6a, 0x60, 0x7a, 0x7a, 0x5c,
	0x6c,
};

static const u8 sw43410_cd[] = {
	0xcd, 0x6c, 0x74, 0x80, 0x7a, 0x79, 0x75, 0x6a,
	0x68, 0x7c, 0x83, 0x83, 0x83, 0x83, 0x8a, 0x8a,
	0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83,
	0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83,
	0x83,
};

static const u8 sw43410_ce[] = {
	0xce, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83,
	0x7a, 0x83, 0x83, 0x83, 0x83, 0x83, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7e, 0x7e, 0x7e,
	0x7f, 0x7f, 0xf4, 0x00, 0x06, 0x02, 0x06, 0x11,
	0xe4,
};

static const u8 sw43410_cf[] = {
	0xcf, 0xef, 0xef, 0xef, 0xf6, 0x06, 0x00, 0x0a,
	0x81, 0xd3, 0xff, 0x4c, 0x50, 0x48, 0x48, 0x4c,
	0x50, 0x50, 0x4a, 0x45, 0x4e, 0x4c, 0x52, 0x54,
	0x54, 0x54, 0x58, 0x58, 0x68, 0x65, 0x6b, 0x53,
	0x5b,
};

static const u8 sw43410_d0[] = {
	0xd0, 0x50, 0x56, 0x62, 0x70, 0x72, 0x74, 0x7b,
	0x7b, 0x7f, 0x80, 0x7b, 0x70, 0x6c, 0x6c, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80,
};

static const u8 sw43410_d1[] = {
	0xd1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x7d, 0x7f, 0x7d, 0x7f, 0x7f, 0x7f, 0x7f, 0xfd,
	0x03,
};

static const u8 sw43410_d2[] = {
	0xd2, 0xff, 0xfd, 0x05, 0x13, 0xff, 0xe7, 0xf6,
	0x06, 0xf6, 0xf6, 0x00, 0x0a, 0x81, 0xd3, 0xff,
	0x48, 0x44, 0x44, 0x4e, 0x53, 0x52, 0x50, 0x50,
	0x4c, 0x60, 0x54, 0x50, 0x4c, 0x50, 0x4c, 0x56,
	0x5a,
};

static const u8 sw43410_d3[] = {
	0xd3, 0x62, 0x5f, 0x60, 0x5d, 0x64, 0x60, 0x70,
	0x63, 0x64, 0x65, 0x6f, 0x6d, 0x7e, 0x78, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80,
};

static const u8 sw43410_d4[] = {
	0xd4, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x7f, 0x7e, 0x7e, 0x7e, 0x7e, 0x7f, 0x7f, 0x7f,
	0x7f,
};

static const u8 sw43410_d5[] = {
	0xd5, 0x7f, 0x7f, 0x7d, 0xfd, 0x0c, 0x11, 0x0c,
	0x04, 0x06, 0xff, 0x08, 0x08, 0x08, 0xfb, 0xe9,
	0x00, 0x0a, 0x81, 0xd3, 0xff, 0x48, 0x46, 0x40,
	0x3a, 0x42, 0x42, 0x4c, 0x4c, 0x3c, 0x38, 0x38,
	0x44,
};

static const u8 sw43410_d6[] = {
	0xd6, 0x4d, 0x54, 0x46, 0x4a, 0x49, 0x58, 0x59,
	0x5d, 0x42, 0x38, 0x3a, 0x4a, 0x62, 0x74, 0x6c,
	0x68, 0x6d, 0x66, 0x74, 0x70, 0x60, 0x54, 0x54,
	0x64, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80,
};

static const u8 sw43410_d7[] = {
	0xd7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x7f, 0x7f, 0x7f,
	0x7f,
};

static const u8 sw43410_d8[] = {
	0xd8, 0x7f, 0x7e, 0x7f, 0x7b, 0x7d, 0x7e, 0x7f,
	0x7f, 0xfd, 0xfb, 0xf4, 0xfb, 0x04, 0x11, 0xfd,
	0xe0, 0xe7, 0xed, 0xfd, 0x01,
};

static const u8 sw43410_ec[] = {
	0xec, 0x3f, 0x03, 0x01, 0x56, 0xab, 0xfc, 0x03,
	0x00, 0x55, 0xaa, 0xfc, 0x03, 0x00, 0x55, 0xaa,
	0xfc, 0x00, 0xff, 0x40, 0x80, 0xc0, 0x00, 0x40,
	0x80, 0xc0, 0x00, 0x40, 0x80, 0xc0, 0x00, 0x40,
	0x80,
};

static const u8 sw43410_ed[] = {
	0xed, 0xc0, 0x00, 0xed, 0x3f, 0x7e, 0xbd, 0xfb,
	0x3a, 0x79, 0xb8, 0xf7, 0x36, 0x75, 0xb4, 0xf2,
	0x31, 0x70, 0xaf, 0x00, 0xd0, 0x3d, 0x7a, 0xb7,
	0xf4, 0x31, 0x6e, 0xab, 0xe8, 0x26, 0x63, 0xa0,
	0xdd,
};

static const u8 sw43410_ee[] = { 0xee, 0x1a, 0x57, 0x94 };

static int sw43410_unprepare(struct drm_panel *panel)
{
	struct sw43410_panel *ctx = to_sw43410(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->link };
	int ret;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x0c, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xca, 0x08, 0x88, 0x10, 0x0f);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 150);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	ret = regulator_bulk_disable(ARRAY_SIZE(sw43410_supplies),
				     ctx->supplies);

	return ret ? : dsi_ctx.accum_err;
}

static int sw43410_program(struct sw43410_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->link };
	struct drm_dsc_picture_parameter_set pps;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xac);
	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3d, 0x08);
	sw43410_write_dsc_config(&dsi_ctx, 0xb9);
	sw43410_write_dsc_config(&dsi_ctx, 0xba);
	sw43410_write_dsc_config(&dsi_ctx, 0xbb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x01, 0x49, 0x0c,
				     0xff, 0x7f, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x26, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x0c, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x55, 0x16, 0x29, 0xc0,
				     0x04, 0x78, 0xdf);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x30, 0x00, 0x00, 0x0c, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x00, 0x00, 0x05, 0x9f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x00, 0x00, 0x0c, 0x2f);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xca);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x55);
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_cc, ARRAY_SIZE(sw43410_cc));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_cd, ARRAY_SIZE(sw43410_cd));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_ce, ARRAY_SIZE(sw43410_ce));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_cf, ARRAY_SIZE(sw43410_cf));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d0, ARRAY_SIZE(sw43410_d0));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d1, ARRAY_SIZE(sw43410_d1));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d2, ARRAY_SIZE(sw43410_d2));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d3, ARRAY_SIZE(sw43410_d3));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d4, ARRAY_SIZE(sw43410_d4));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d5, ARRAY_SIZE(sw43410_d5));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d6, ARRAY_SIZE(sw43410_d6));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d7, ARRAY_SIZE(sw43410_d7));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_d8, ARRAY_SIZE(sw43410_d8));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_ec, ARRAY_SIZE(sw43410_ec));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_ed, ARRAY_SIZE(sw43410_ed));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, sw43410_ee, ARRAY_SIZE(sw43410_ee));
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xac);

	ctx->link->mode_flags &= ~MIPI_DSI_MODE_LPM;
	drm_dsc_pps_payload_pack(&pps, ctx->link->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	ctx->link->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xac);
	mipi_dsi_msleep(&dsi_ctx, 30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb8, 0x3d, 0x01, 0x1f,
				     0x01, 0xff, 0x3c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbf, 0x30, 0x0f, 0x06,
				     0x11, 0x22, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x03, 0x03,
				     0x33, 0x00, 0x01, 0x00, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd0, 0xec, 0x21, 0x23,
				     0x1f, 0x21, 0x24, 0x00, 0x00, 0x00,
				     0x6c, 0x26, 0x00, 0x00, 0x00, 0x08,
				     0x00, 0x84, 0x91, 0x91, 0x1d, 0x1d,
				     0x06, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2, 0xfc, 0x0c, 0x30,
				     0x00, 0x00, 0x0a, 0xaa, 0x3b, 0xfc,
				     0x40, 0x44, 0x8e, 0x00, 0x00, 0xa4,
				     0x00, 0xb0, 0x01, 0x00, 0x10, 0x00,
				     0x10, 0x60, 0xb0, 0x00, 0x00, 0x00,
				     0x11, 0x23, 0x34, 0x46, 0x58);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3, 0x02, 0x54, 0xf2,
				     0xad, 0xb2, 0xb4, 0xb0, 0xa8, 0xaf,
				     0xb8, 0x01, 0x20);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xac);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	return dsi_ctx.accum_err;
}

static void sw43410_reset(struct sw43410_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int sw43410_prepare(struct drm_panel *panel)
{
	struct sw43410_panel *ctx = to_sw43410(panel);
	int ret;

	ret = regulator_enable(ctx->supplies[0].consumer);
	if (ret < 0)
		return ret;

	msleep(20);
	ret = regulator_enable(ctx->supplies[1].consumer);
	if (ret < 0) {
		regulator_disable(ctx->supplies[0].consumer);
		return ret;
	}

	sw43410_reset(ctx);

	ret = sw43410_program(ctx);
	if (ret)
		goto poweroff;

	return 0;

poweroff:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(sw43410_supplies), ctx->supplies);
	return ret;
}

static const struct drm_display_mode sw43410_mode = {
	.clock = (1440 + 168 + 4 + 88) * (3120 + 9 + 1 + 10) * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 168,
	.hsync_end = 1440 + 168 + 4,
	.htotal = 1440 + 168 + 4 + 88,
	.vdisplay = 3120,
	.vsync_start = 3120 + 9,
	.vsync_end = 3120 + 9 + 1,
	.vtotal = 3120 + 9 + 1 + 10,
	.width_mm = 65,
	.height_mm = 140,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int sw43410_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &sw43410_mode);
}

static int sw43410_backlight_update_status(struct backlight_device *backlight)
{
	struct mipi_dsi_device *dsi = bl_get_data(backlight);
	u16 brightness = backlight_get_brightness(backlight);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret;
}

static const struct backlight_ops sw43410_backlight_ops = {
	.update_status = sw43410_backlight_update_status,
};

static int sw43410_backlight_init(struct sw43410_panel *ctx)
{
	struct device *dev = &ctx->link->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 158,
		.max_brightness = 4095,
	};

	ctx->base.backlight = devm_backlight_device_register(dev, dev_name(dev),
							     dev, ctx->link,
							     &sw43410_backlight_ops,
							     &props);
	if (IS_ERR(ctx->base.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->base.backlight),
				     "Failed to create backlight\n");

	return 0;
}

static const struct drm_panel_funcs sw43410_panel_funcs = {
	.prepare = sw43410_prepare,
	.unprepare = sw43410_unprepare,
	.get_modes = sw43410_get_modes,
};

static const struct of_device_id sw43410_of_match[] = {
	{ .compatible = "lg,sw43410" },
	{}
};
MODULE_DEVICE_TABLE(of, sw43410_of_match);

static int sw43410_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct sw43410_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct sw43410_panel, base,
				   &sw43410_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(sw43410_supplies),
					    sw43410_supplies, &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->link = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM;

	ret = sw43410_backlight_init(ctx);
	if (ret < 0)
		return ret;

	ctx->base.prepare_prev_first = true;
	drm_panel_add(&ctx->base);

	dsi->dsc = &ctx->dsc;
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.scr_rev = 1;
	ctx->dsc.slice_height = 60;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->base);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void sw43410_remove(struct mipi_dsi_device *dsi)
{
	struct sw43410_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->base);
}

static struct mipi_dsi_driver sw43410_driver = {
	.probe = sw43410_probe,
	.remove = sw43410_remove,
	.driver = {
		.name = "panel-lg-sw43410",
		.of_match_table = sw43410_of_match,
	},
};
module_mipi_dsi_driver(sw43410_driver);

MODULE_DESCRIPTION("LG SW43410 MIPI-DSI DSC panel");
MODULE_LICENSE("GPL");
