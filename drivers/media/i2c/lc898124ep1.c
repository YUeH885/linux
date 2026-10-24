// SPDX-License-Identifier: GPL-2.0-only
/*
 * onsemi LC898124EP1 Optical Image Stabilization (OIS) controller driver
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define LC898124EP1_REG_MODE	0xf100
#define LC898124EP1_REG_ENABLE	0xf012

static const char * const lc898124ep1_supply_names[] = {
	"vdd",
	"vio",
};

struct lc898124ep1 {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *stabilization;
	struct regmap *regmap;
	struct regulator_bulk_data supplies[ARRAY_SIZE(lc898124ep1_supply_names)];
};

static int lc898124ep1_set_stabilization(struct lc898124ep1 *ois, bool enable)
{
	int ret = 0;

	cci_write(ois->regmap, CCI_REG8(LC898124EP1_REG_MODE), 0, &ret);
	cci_write(ois->regmap, CCI_REG32(LC898124EP1_REG_ENABLE),
		  enable ? 1 : 0, &ret);

	return ret;
}

static int lc898124ep1_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lc898124ep1 *ois = container_of(ctrl->handler,
					       struct lc898124ep1, controls);
	int ret;

	if (ctrl->id != V4L2_CID_IMAGE_STABILIZATION)
		return -EINVAL;

	ret = pm_runtime_get_if_active(ois->sd.dev);
	if (ret <= 0)
		return ret;

	ret = lc898124ep1_set_stabilization(ois, ctrl->val != 0);

	pm_runtime_put(ois->sd.dev);
	return ret;
}

static const struct v4l2_ctrl_ops lc898124ep1_ctrl_ops = {
	.s_ctrl = lc898124ep1_set_ctrl,
};

static int lc898124ep1_open(struct v4l2_subdev *sd,
			    struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int lc898124ep1_close(struct v4l2_subdev *sd,
			     struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops lc898124ep1_internal_ops = {
	.open = lc898124ep1_open,
	.close = lc898124ep1_close,
};

static const struct v4l2_subdev_ops lc898124ep1_subdev_ops = { };

static int lc898124ep1_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898124ep1 *ois = container_of(sd, struct lc898124ep1, sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ois->supplies), ois->supplies);
	if (ret)
		return ret;

	usleep_range(20000, 21000);

	ret = lc898124ep1_set_stabilization(ois, ois->stabilization->val != 0);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(ois->supplies), ois->supplies);
		return ret;
	}

	return 0;
}

static int lc898124ep1_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898124ep1 *ois = container_of(sd, struct lc898124ep1, sd);

	lc898124ep1_set_stabilization(ois, false);
	usleep_range(2000, 3000);

	return regulator_bulk_disable(ARRAY_SIZE(ois->supplies), ois->supplies);
}

static int lc898124ep1_probe(struct i2c_client *client)
{
	struct lc898124ep1 *ois;
	unsigned int i;
	int ret;

	ois = devm_kzalloc(&client->dev, sizeof(*ois), GFP_KERNEL);
	if (!ois)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(ois->supplies); i++)
		ois->supplies[i].supply = lc898124ep1_supply_names[i];

	ret = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(ois->supplies),
				      ois->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret, "Failed to get supplies\n");

	ois->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ois->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(ois->regmap),
				     "Failed to initialize CCI\n");

	v4l2_i2c_subdev_init(&ois->sd, client, &lc898124ep1_subdev_ops);
	ois->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ois->sd.internal_ops = &lc898124ep1_internal_ops;
	ois->sd.entity.function = MEDIA_ENT_F_LENS;

	v4l2_ctrl_handler_init(&ois->controls, 1);
	ois->stabilization = v4l2_ctrl_new_std(&ois->controls,
					       &lc898124ep1_ctrl_ops,
					       V4L2_CID_IMAGE_STABILIZATION,
					       0, 1, 1, 0);
	if (ois->controls.error) {
		ret = ois->controls.error;
		goto free_controls;
	}
	ois->sd.ctrl_handler = &ois->controls;

	ret = media_entity_pads_init(&ois->sd.entity, 0, NULL);
	if (ret)
		goto free_controls;

	pm_runtime_enable(&client->dev);
	ret = v4l2_async_register_subdev(&ois->sd);
	if (ret)
		goto cleanup_entity;

	return 0;

cleanup_entity:
	pm_runtime_disable(&client->dev);
	media_entity_cleanup(&ois->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&ois->controls);
	return ret;
}

static void lc898124ep1_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898124ep1 *ois = container_of(sd, struct lc898124ep1, sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		lc898124ep1_suspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ois->controls);
}

static DEFINE_RUNTIME_DEV_PM_OPS(lc898124ep1_pm_ops, lc898124ep1_suspend,
				 lc898124ep1_resume, NULL);

static const struct of_device_id lc898124ep1_of_match[] = {
	{ .compatible = "onnn,lc898124ep1" },
	{ }
};
MODULE_DEVICE_TABLE(of, lc898124ep1_of_match);

static struct i2c_driver lc898124ep1_driver = {
	.driver = {
		.name = "lc898124ep1",
		.pm = pm_ptr(&lc898124ep1_pm_ops),
		.of_match_table = lc898124ep1_of_match,
	},
	.probe = lc898124ep1_probe,
	.remove = lc898124ep1_remove,
};
module_i2c_driver(lc898124ep1_driver);

MODULE_DESCRIPTION("onsemi LC898124EP1 optical image stabilization driver");
MODULE_LICENSE("GPL");
