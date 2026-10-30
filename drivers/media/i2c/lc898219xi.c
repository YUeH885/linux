// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define LC898219XI_REG_POSITION	CCI_REG16(0x84)
#define LC898219XI_REG_INITIALIZE	CCI_REG8(0xe0)
#define LC898219XI_REG_TEMPERATURE	CCI_REG16(0x58)
#define LC898219XI_MIN_POSITION	(-2048)
#define LC898219XI_MAX_POSITION	2047

static const char * const lc898219xi_supply_names[] = {
	"vdd",
	"vio",
};

struct lc898219xi {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *focus;
	struct regmap *regmap;
	u32 temperature_shift;
	struct regulator_bulk_data supplies[ARRAY_SIZE(lc898219xi_supply_names)];
};

static int lc898219xi_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lc898219xi *lens = container_of(ctrl->handler,
					       struct lc898219xi, controls);
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	if (pm_runtime_get_if_in_use(lens->sd.dev) <= 0)
		return 0;

	ret = cci_write(lens->regmap, LC898219XI_REG_POSITION,
			ctrl->val & 0xfff, NULL);
	pm_runtime_put(lens->sd.dev);

	return ret;
}

static const struct v4l2_ctrl_ops lc898219xi_ctrl_ops = {
	.s_ctrl = lc898219xi_set_ctrl,
};

static int lc898219xi_open(struct v4l2_subdev *sd,
			   struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int lc898219xi_close(struct v4l2_subdev *sd,
			    struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops lc898219xi_internal_ops = {
	.open = lc898219xi_open,
	.close = lc898219xi_close,
};

static const struct v4l2_subdev_ops lc898219xi_subdev_ops = { };

static int lc898219xi_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898219xi *lens = container_of(sd, struct lc898219xi, sd);
	u64 temperature;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(lens->supplies), lens->supplies);
	if (ret)
		return ret;

	usleep_range(20000, 21000);
	ret = 0;
	cci_write(lens->regmap, CCI_REG8(0x8e), 0x15, &ret);
	cci_write(lens->regmap, CCI_REG8(0x8d), 0x20, &ret);
	cci_write(lens->regmap, CCI_REG8(0x81), 0x80, &ret);
	if (ret)
		goto disable_supplies;

	usleep_range(1000, 2000);
	ret = cci_read(lens->regmap, LC898219XI_REG_TEMPERATURE,
		       &temperature, NULL);
	if (ret)
		goto disable_supplies;

	ret = cci_write(lens->regmap, LC898219XI_REG_INITIALIZE, 1, NULL);
	if (ret)
		goto disable_supplies;

	usleep_range(1000, 2000);
	temperature = (u16)(temperature << lens->temperature_shift);
	cci_write(lens->regmap, CCI_REG16(0x30), temperature, &ret);
	cci_write(lens->regmap, CCI_REG16(0x32), temperature, &ret);
	cci_write(lens->regmap, CCI_REG16(0x76), temperature, &ret);
	cci_write(lens->regmap, CCI_REG16(0x78), temperature, &ret);
	cci_write(lens->regmap, CCI_REG8(0x8c), 0xe9, &ret);
	if (ret)
		goto disable_supplies;

	ret = cci_write(lens->regmap, LC898219XI_REG_POSITION,
			lens->focus->val & 0xfff, NULL);
	if (ret)
		goto disable_supplies;

	return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(lens->supplies), lens->supplies);
	return ret;
}

static int lc898219xi_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898219xi *lens = container_of(sd, struct lc898219xi, sd);
	int ret;

	ret = regulator_bulk_disable(ARRAY_SIZE(lens->supplies), lens->supplies);
	usleep_range(1000, 2000);
	return ret;
}

static int lc898219xi_probe(struct i2c_client *client)
{
	struct lc898219xi *lens;
	unsigned int i;
	int ret;

	lens = devm_kzalloc(&client->dev, sizeof(*lens), GFP_KERNEL);
	if (!lens)
		return -ENOMEM;

	ret = device_property_read_u32(&client->dev, "onnn,temperature-shift",
				       &lens->temperature_shift);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to read temperature shift\n");
	if (lens->temperature_shift != 0 && lens->temperature_shift != 1 &&
	    lens->temperature_shift != 3)
		return dev_err_probe(&client->dev, -EINVAL,
				     "Invalid temperature shift\n");

	for (i = 0; i < ARRAY_SIZE(lens->supplies); i++)
		lens->supplies[i].supply = lc898219xi_supply_names[i];

	ret = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(lens->supplies),
				      lens->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret, "Failed to get supplies\n");

	lens->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(lens->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(lens->regmap),
				     "Failed to initialize CCI\n");

	v4l2_i2c_subdev_init(&lens->sd, client, &lc898219xi_subdev_ops);
	lens->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	lens->sd.internal_ops = &lc898219xi_internal_ops;
	lens->sd.entity.function = MEDIA_ENT_F_LENS;

	v4l2_ctrl_handler_init(&lens->controls, 1);
	/* The LM-G820N factory profile starts at DAC code 100. */
	lens->focus = v4l2_ctrl_new_std(&lens->controls, &lc898219xi_ctrl_ops,
					V4L2_CID_FOCUS_ABSOLUTE,
					LC898219XI_MIN_POSITION,
					LC898219XI_MAX_POSITION, 1, 100);
	if (lens->controls.error) {
		ret = lens->controls.error;
		goto free_controls;
	}
	lens->sd.ctrl_handler = &lens->controls;

	ret = media_entity_pads_init(&lens->sd.entity, 0, NULL);
	if (ret)
		goto free_controls;

	pm_runtime_enable(&client->dev);
	ret = v4l2_async_register_subdev(&lens->sd);
	if (ret)
		goto cleanup_entity;

	return 0;

cleanup_entity:
	pm_runtime_disable(&client->dev);
	media_entity_cleanup(&lens->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&lens->controls);
	return ret;
}

static void lc898219xi_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898219xi *lens = container_of(sd, struct lc898219xi, sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		lc898219xi_suspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&lens->controls);
}

static DEFINE_RUNTIME_DEV_PM_OPS(lc898219xi_pm_ops, lc898219xi_suspend,
				 lc898219xi_resume, NULL);

static const struct of_device_id lc898219xi_of_match[] = {
	{ .compatible = "onnn,lc898219xi" },
	{ }
};
MODULE_DEVICE_TABLE(of, lc898219xi_of_match);

static struct i2c_driver lc898219xi_driver = {
	.driver = {
		.name = "lc898219xi",
		.pm = pm_ptr(&lc898219xi_pm_ops),
		.of_match_table = lc898219xi_of_match,
	},
	.probe = lc898219xi_probe,
	.remove = lc898219xi_remove,
};
module_i2c_driver(lc898219xi_driver);

MODULE_DESCRIPTION("onsemi LC898219XI lens actuator driver");
MODULE_LICENSE("GPL");
