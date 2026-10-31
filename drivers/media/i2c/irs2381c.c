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

#define IRS2381C_LINK_FREQ_400MHZ	400000000ULL
#define IRS2381C_MCLK_FREQ_24MHZ	(24000 * HZ_PER_KHZ)
#define IRS2381C_DATA_LANES		2

#define IRS2381C_REG_CHIP_ID		CCI_REG16(0x0000)
#define IRS2381C_CHIP_ID		0x0300

#define IRS2381C_NUM_SUPPLIES		3

#define to_irs2381c(_sd)		container_of(_sd, struct irs2381c, sd)

static const char * const irs2381c_supply_names[IRS2381C_NUM_SUPPLIES] = {
	"vddio",
	"vdda",
	"vddd",
};

static const s64 irs2381c_link_freq_menu[] = {
	IRS2381C_LINK_FREQ_400MHZ,
};

static const u32 irs2381c_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR12_1X12,
};

struct irs2381c_mode {
	u32 width;
	u32 height;
	u32 flash_offset;
};

static const struct irs2381c_mode irs2381c_supported_modes[] = {
	{
		.width = 224,
		.height = 1903,
		.flash_offset = 0x2a00,
	},
};

struct irs2381c {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IRS2381C_NUM_SUPPLIES];

	const struct irs2381c_mode *cur_mode;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;

	bool streaming;
};

static const struct media_entity_operations irs2381c_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct cci_reg_sequence irs2381c_initial_config[] = {
	/* ZWETSCHGE EEPROM eLENA initialization map. */
	{ CCI_REG16(0xa001), 0x0007 },
	{ CCI_REG16(0xa008), 0x1513 },
	{ CCI_REG16(0xa00c), 0x0135 },
	{ CCI_REG16(0x9401), 0x0002 },
	{ CCI_REG16(0xa087), 0x8003 },
	{ CCI_REG16(0xa039), 0x16a1 },
	{ CCI_REG16(0xa03a), 0x5555 },
	{ CCI_REG16(0xa03b), 0x0005 },
	{ CCI_REG16(0xa03c), 0x0000 },
	{ CCI_REG16(0xa03d), 0x04d0 },
	{ CCI_REG16(0xa03e), 0x0000 },
	{ CCI_REG16(0xa03f), 0x0017 },
};

static int irs2381c_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct irs2381c *irs2381c = to_irs2381c(sd);
	int ret;

	gpiod_set_value_cansleep(irs2381c->reset_gpio, 1);

	ret = regulator_bulk_enable(IRS2381C_NUM_SUPPLIES, irs2381c->supplies);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}
	usleep_range(5000, 7000);

	ret = clk_prepare_enable(irs2381c->mclk);
	if (ret) {
		dev_err(dev, "failed to enable MCLK clock: %d\n", ret);
		goto disable_supplies;
	}
	usleep_range(5000, 7000);

	gpiod_set_value_cansleep(irs2381c->reset_gpio, 0);
	usleep_range(10000, 15000);

	return 0;

disable_supplies:
	regulator_bulk_disable(IRS2381C_NUM_SUPPLIES, irs2381c->supplies);
	return ret;
}

static int irs2381c_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct irs2381c *irs2381c = to_irs2381c(sd);

	gpiod_set_value_cansleep(irs2381c->reset_gpio, 1);
	usleep_range(1000, 2000);
	clk_disable_unprepare(irs2381c->mclk);
	regulator_bulk_disable(IRS2381C_NUM_SUPPLIES, irs2381c->supplies);

	return 0;
}

static int irs2381c_identify_sensor(struct irs2381c *irs2381c)
{
	u64 val;
	int ret;

	ret = cci_read(irs2381c->regmap, IRS2381C_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(irs2381c->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != IRS2381C_CHIP_ID) {
		dev_err(irs2381c->dev, "chip id mismatch: %x!=%llx\n",
			IRS2381C_CHIP_ID, val);
		return -ENODEV;
	}

	return 0;
}

static int irs2381c_start_streaming(struct irs2381c *irs2381c)
{
	u64 status;
	int ret;

	ret = cci_multi_reg_write(irs2381c->regmap, irs2381c_initial_config,
				  ARRAY_SIZE(irs2381c_initial_config), NULL);
	if (ret)
		return ret;

	cci_write(irs2381c->regmap, CCI_REG16(0xa02a), 0x0001, &ret);

	/* ImagerM2453_B11 copies the selected use case from module Flash. */
	cci_write(irs2381c->regmap, CCI_REG16(0xa087), 0x4002, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x9380),
		  irs2381c->cur_mode->flash_offset & 0xffff, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x9381),
		  irs2381c->cur_mode->flash_offset >> 16, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x93e1), 0x0007, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x8452), 0x0004, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x8401), 0x0008, &ret);
	if (ret)
		return ret;
	usleep_range(10000, 12000);

	cci_read(irs2381c->regmap, CCI_REG16(0x93e2), &status, &ret);
	if (ret)
		return ret;
	if (status != 0xaaaa) {
		dev_err(irs2381c->dev, "use case transfer failed: 0x%04llx\n", status);
		return -EIO;
	}

	return cci_write(irs2381c->regmap, CCI_REG16(0x9400), 0x0001, NULL);
}

static int irs2381c_stop_streaming(struct irs2381c *irs2381c)
{
	int ret;

	ret = cci_write(irs2381c->regmap, CCI_REG16(0x9400), 0x0000, NULL);
	if (!ret)
		usleep_range(10000, 15000);

	return ret;
}

static int irs2381c_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (irs2381c->streaming == enable)
		goto unlock;

	if (enable) {
		ret = pm_runtime_resume_and_get(irs2381c->dev);
		if (ret < 0)
			goto unlock;

		ret = irs2381c_start_streaming(irs2381c);
		if (ret) {
			pm_runtime_put_autosuspend(irs2381c->dev);
			goto unlock;
		}
	} else {
		ret = irs2381c_stop_streaming(irs2381c);
		if (ret) {
			dev_err(irs2381c->dev, "failed to stop streaming: %d\n", ret);
			pm_runtime_put_sync_suspend(irs2381c->dev);
		} else {
			pm_runtime_mark_last_busy(irs2381c->dev);
			pm_runtime_put_autosuspend(irs2381c->dev);
		}
	}

	irs2381c->streaming = enable;

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static int irs2381c_init_controls(struct irs2381c *irs2381c)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &irs2381c->ctrl_handler;
	s64 pixel_rate_val;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 2);

	irs2381c->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL,
						     V4L2_CID_LINK_FREQ,
						     ARRAY_SIZE(irs2381c_link_freq_menu) - 1,
						     0, irs2381c_link_freq_menu);
	if (irs2381c->link_freq)
		irs2381c->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate_val = (IRS2381C_LINK_FREQ_400MHZ * 2 * IRS2381C_DATA_LANES) / 12;
	irs2381c->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
						 V4L2_CID_PIXEL_RATE, pixel_rate_val,
						 pixel_rate_val, 1, pixel_rate_val);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(irs2381c->dev, "controls initialization failed: %d\n", ret);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ret;
	}

	irs2381c->sd.ctrl_handler = ctrl_hdlr;
	return 0;
}

static int irs2381c_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(irs2381c_mbus_formats))
		return -EINVAL;

	code->code = irs2381c_mbus_formats[code->index];
	return 0;
}

static int irs2381c_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(irs2381c_supported_modes))
		return -EINVAL;

	fse->min_width = irs2381c_supported_modes[fse->index].width;
	fse->max_width = irs2381c_supported_modes[fse->index].width;
	fse->min_height = irs2381c_supported_modes[fse->index].height;
	fse->max_height = irs2381c_supported_modes[fse->index].height;

	return 0;
}

static int irs2381c_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	const struct irs2381c_mode *mode;
	unsigned int i;

	mode = v4l2_find_nearest_size(irs2381c_supported_modes,
				      ARRAY_SIZE(irs2381c_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	for (i = 0; i < ARRAY_SIZE(irs2381c_mbus_formats); i++) {
		if (fmt->format.code == irs2381c_mbus_formats[i])
			break;
	}
	if (i == ARRAY_SIZE(irs2381c_mbus_formats))
		i = 0;

	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = irs2381c_mbus_formats[i];
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		irs2381c->cur_mode = mode;

	return 0;
}

static int irs2381c_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	const struct v4l2_mbus_framefmt *fmt;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		fmt = v4l2_subdev_state_get_format(sd_state, sel->pad);
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = fmt->width;
		sel->r.height = fmt->height;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = irs2381c_supported_modes[0].width;
		sel->r.height = irs2381c_supported_modes[0].height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int irs2381c_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.width = irs2381c_supported_modes[0].width,
			.height = irs2381c_supported_modes[0].height,
			.code = irs2381c_mbus_formats[0],
		},
	};

	return irs2381c_set_fmt(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_video_ops irs2381c_video_ops = {
	.s_stream = irs2381c_set_stream,
};

static const struct v4l2_subdev_pad_ops irs2381c_pad_ops = {
	.enum_mbus_code = irs2381c_enum_mbus_code,
	.enum_frame_size = irs2381c_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = irs2381c_set_fmt,
	.get_selection = irs2381c_get_selection,
};

static const struct v4l2_subdev_ops irs2381c_subdev_ops = {
	.video = &irs2381c_video_ops,
	.pad = &irs2381c_pad_ops,
};

static const struct v4l2_subdev_internal_ops irs2381c_internal_ops = {
	.init_state = irs2381c_init_state,
};

static int irs2381c_check_hwcfg(struct irs2381c *irs2381c)
{
	struct fwnode_handle *fwnode = dev_fwnode(irs2381c->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus = {
			.mipi_csi2 = {
				.num_data_lanes = IRS2381C_DATA_LANES,
			},
		},
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENXIO;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IRS2381C_DATA_LANES) {
		dev_err(irs2381c->dev, "unsupported number of data-lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(irs2381c->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       irs2381c_link_freq_menu,
				       ARRAY_SIZE(irs2381c_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int irs2381c_probe(struct i2c_client *client)
{
	struct irs2381c *irs2381c;
	unsigned long freq;
	unsigned int i;
	int ret;

	irs2381c = devm_kzalloc(&client->dev, sizeof(*irs2381c), GFP_KERNEL);
	if (!irs2381c)
		return -ENOMEM;

	irs2381c->dev = &client->dev;
	v4l2_i2c_subdev_init(&irs2381c->sd, client, &irs2381c_subdev_ops);

	irs2381c->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(irs2381c->regmap))
		return dev_err_probe(irs2381c->dev, PTR_ERR(irs2381c->regmap),
				     "failed to init CCI regmap\n");

	irs2381c->mclk = devm_v4l2_sensor_clk_get(irs2381c->dev, NULL);
	if (IS_ERR(irs2381c->mclk))
		return dev_err_probe(irs2381c->dev, PTR_ERR(irs2381c->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(irs2381c->mclk);
	if (freq != IRS2381C_MCLK_FREQ_24MHZ)
		return dev_err_probe(irs2381c->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = irs2381c_check_hwcfg(irs2381c);
	if (ret)
		return dev_err_probe(irs2381c->dev, ret,
				     "failed to check HW configuration\n");

	irs2381c->reset_gpio = devm_gpiod_get_optional(irs2381c->dev, "reset",
						       GPIOD_OUT_HIGH);
	if (IS_ERR(irs2381c->reset_gpio))
		return dev_err_probe(irs2381c->dev, PTR_ERR(irs2381c->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < IRS2381C_NUM_SUPPLIES; i++)
		irs2381c->supplies[i].supply = irs2381c_supply_names[i];

	ret = devm_regulator_bulk_get(irs2381c->dev, IRS2381C_NUM_SUPPLIES,
				      irs2381c->supplies);
	if (ret)
		return dev_err_probe(irs2381c->dev, ret,
				     "failed to get supply regulators\n");

	irs2381c->cur_mode = &irs2381c_supported_modes[0];

	ret = irs2381c_power_on(irs2381c->dev);
	if (ret)
		return ret;

	ret = irs2381c_identify_sensor(irs2381c);
	if (ret) {
		dev_err_probe(irs2381c->dev, ret, "failed to identify IRS2381C\n");
		goto power_off;
	}

	ret = irs2381c_init_controls(irs2381c);
	if (ret) {
		dev_err_probe(irs2381c->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	irs2381c->sd.state_lock = irs2381c->ctrl_handler.lock;
	irs2381c->sd.internal_ops = &irs2381c_internal_ops;
	irs2381c->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	irs2381c->sd.entity.ops = &irs2381c_subdev_entity_ops;
	irs2381c->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	irs2381c->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&irs2381c->sd.entity, 1, &irs2381c->pad);
	if (ret) {
		dev_err_probe(irs2381c->dev, ret,
			      "failed to init media entity pads\n");
		goto free_ctrls;
	}

	ret = v4l2_subdev_init_finalize(&irs2381c->sd);
	if (ret < 0) {
		dev_err_probe(irs2381c->dev, ret,
			      "failed to finalize V4L2 subdev\n");
		goto cleanup_entity;
	}

	pm_runtime_set_active(irs2381c->dev);
	pm_runtime_enable(irs2381c->dev);

	ret = v4l2_async_register_subdev_sensor(&irs2381c->sd);
	if (ret < 0) {
		dev_err_probe(irs2381c->dev, ret,
			      "failed to register V4L2 sensor subdev\n");
		goto disable_pm;
	}

	pm_runtime_set_autosuspend_delay(irs2381c->dev, 1000);
	pm_runtime_use_autosuspend(irs2381c->dev);
	pm_runtime_idle(irs2381c->dev);

	return 0;

disable_pm:
	v4l2_subdev_cleanup(&irs2381c->sd);
	pm_runtime_disable(irs2381c->dev);
	pm_runtime_set_suspended(irs2381c->dev);
cleanup_entity:
	media_entity_cleanup(&irs2381c->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(irs2381c->sd.ctrl_handler);
power_off:
	irs2381c_power_off(irs2381c->dev);
	return ret;
}

static void irs2381c_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct irs2381c *irs2381c = to_irs2381c(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(irs2381c->dev);
	if (!pm_runtime_status_suspended(irs2381c->dev))
		irs2381c_power_off(irs2381c->dev);
	pm_runtime_set_suspended(irs2381c->dev);
}

static const struct dev_pm_ops irs2381c_pm_ops = {
	SET_RUNTIME_PM_OPS(irs2381c_power_off, irs2381c_power_on, NULL)
};

static const struct of_device_id irs2381c_of_match[] = {
	{ .compatible = "infineon,irs2381c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, irs2381c_of_match);

static struct i2c_driver irs2381c_i2c_driver = {
	.driver = {
		.name = "irs2381c",
		.pm = &irs2381c_pm_ops,
		.of_match_table = irs2381c_of_match,
	},
	.probe = irs2381c_probe,
	.remove = irs2381c_remove,
};

module_i2c_driver(irs2381c_i2c_driver);

MODULE_DESCRIPTION("Infineon REAL3 IRS2381C ToF sensor driver");
MODULE_AUTHOR("The LM-G820N Linux contributors");
MODULE_LICENSE("GPL");
