// SPDX-License-Identifier: GPL-2.0-only
/*
 * onsemi LC898124EP1 Optical Image Stabilization (OIS) controller driver
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define LC898124EP1_REG_MODE	0xf013
#define LC898124EP1_REG_ENABLE	0xf012
#define LC898124EP1_REG_CENTER	0xf010
#define LC898124EP1_REG_STATUS	0xf100
#define LC898124EP1_REG_IO_ADDR	0xc000
#define LC898124EP1_REG_IO_DATA	0xd000
#define LC898124EP1_ROMINFO	0xe0500c
#define LC898124EP1_CORE_RESET	0xd000ac
#define LC898124EP1_PM_CHECKSUM	0x0514
#define LC898124EP1_DM_CHECKSUM	0x0540
#define LC898124EP1_DM_COMMAND	0x8588
#define LC898124EP1_FW_PREFIX	"onnn/lc898124ep1-lgit-"

static const char * const lc898124ep1_supply_names[] = {
	"vdd",
	"vio",
	"vmotor",
};

struct lc898124ep1 {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *stabilization;
	struct regmap *regmap;
	u8 firmware_variant;
	struct regulator_bulk_data supplies[ARRAY_SIZE(lc898124ep1_supply_names)];
};

static int lc898124ep1_send(struct lc898124ep1 *ois, const u8 *data, size_t size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ois->sd);
	int ret = i2c_master_send(client, data, size);

	return ret < 0 ? ret : (ret == size ? 0 : -EIO);
}

static int lc898124ep1_wait_ready(struct lc898124ep1 *ois)
{
	u64 status;
	int ret, read_ret;

	ret = read_poll_timeout(cci_read, read_ret, read_ret || !status,
				5000, 75000, false, ois->regmap,
				CCI_REG32(LC898124EP1_REG_STATUS), &status, NULL);
	return read_ret ? read_ret : ret;
}

static int lc898124ep1_command(struct lc898124ep1 *ois, u16 reg, u32 value)
{
	int ret;

	ret = cci_write(ois->regmap, CCI_REG32(reg), value, NULL);
	if (ret)
		return ret;

	return lc898124ep1_wait_ready(ois);
}

static int lc898124ep1_download(struct lc898124ep1 *ois,
				const struct firmware *pm, const struct firmware *dm)
{
	u8 pm_start[] = { 0x30, 0x00, 0x10, 0x00, 0x00 };
	u8 pm_check[] = { 0xf0, 0x0a, pm->size / 5 >> 8, pm->size / 5 };
	u8 pm_word[6] = { 0x40 };
	const u8 *dm_command = NULL;
	u64 rom, pm_sum, dm_sum, version, actuator;
	u32 expected = ois->firmware_variant == 1 ? 0xa7350880 : 0xaa9f0742;
	size_t offset;
	int ret = 0;

	cci_write(ois->regmap, CCI_REG32(LC898124EP1_REG_IO_ADDR),
		  LC898124EP1_ROMINFO, &ret);
	cci_read(ois->regmap, CCI_REG32(LC898124EP1_REG_IO_DATA), &rom, &ret);
	if (ret)
		return ret;

	if ((rom & 0xff) == 1) {
		cci_write(ois->regmap, CCI_REG32(LC898124EP1_REG_IO_ADDR),
			  LC898124EP1_CORE_RESET, &ret);
		cci_write(ois->regmap, CCI_REG32(LC898124EP1_REG_IO_DATA),
			  0x1000, &ret);
		if (ret)
			return ret;
		usleep_range(6000, 7000);
	} else if ((rom & 0xff) != 0x0a) {
		dev_err(ois->sd.dev, "Unexpected boot ROM status: %#llx\n", rom);
		return -EPROTO;
	}

	ret = lc898124ep1_send(ois, pm_start, sizeof(pm_start));
	if (ret)
		return ret;

	/* A five-byte PM word plus its command fits the CCI write FIFO. */
	for (offset = 0; offset < pm->size; offset += 5) {
		memcpy(pm_word + 1, pm->data + offset, 5);
		ret = lc898124ep1_send(ois, pm_word, sizeof(pm_word));
		if (ret)
			return ret;
	}
	ret = lc898124ep1_send(ois, pm_check, sizeof(pm_check));
	if (ret)
		return ret;

	cci_write(ois->regmap, CCI_REG32(LC898124EP1_DM_CHECKSUM), 0, &ret);
	cci_write(ois->regmap, CCI_REG32(LC898124EP1_DM_COMMAND), 0x1001de, &ret);
	if (ret)
		return ret;
	for (offset = 0; offset < dm->size; offset += 6) {
		/* Keep the checksum handler active across separate I2C writes. */
		if (dm->data[offset] == 0x85 && dm->data[offset + 1] == 0x88) {
			dm_command = dm->data + offset;
			continue;
		}
		ret = lc898124ep1_send(ois, dm->data + offset, 6);
		if (ret)
			return ret;
	}
	if (!dm_command)
		return -EINVAL;
	ret = lc898124ep1_send(ois, dm_command, 6);
	if (ret)
		return ret;
	cci_read(ois->regmap, CCI_REG32(LC898124EP1_PM_CHECKSUM), &pm_sum, &ret);
	cci_read(ois->regmap, CCI_REG32(LC898124EP1_DM_CHECKSUM), &dm_sum, &ret);
	cci_write(ois->regmap, CCI_REG32(LC898124EP1_DM_COMMAND), 0x418c4, &ret);
	if (ret)
		return ret;
	if ((u32)(pm_sum + dm_sum) != expected) {
		dev_err(ois->sd.dev, "Firmware checksum mismatch: PM %#llx DM %#llx\n",
			pm_sum, dm_sum);
		return -EBADMSG;
	}

	ret = cci_write(ois->regmap, CCI_REG32(0xf000), 0, NULL);
	if (ret)
		return ret;
	ret = lc898124ep1_wait_ready(ois);
	if (ret)
		return ret;
	cci_read(ois->regmap, CCI_REG32(0x8000), &version, &ret);
	cci_read(ois->regmap, CCI_REG32(0x8004), &actuator, &ret);
	cci_write(ois->regmap, CCI_REG32(LC898124EP1_REG_IO_ADDR),
		  LC898124EP1_ROMINFO, &ret);
	cci_read(ois->regmap, CCI_REG32(LC898124EP1_REG_IO_DATA), &rom, &ret);
	if (ret)
		return ret;
	if ((rom & 0xff) != 1 || version != 0x090e000b ||
	    actuator != ois->firmware_variant << 8)
		return -EPROTO;

	dev_info(ois->sd.dev, "DSP firmware %#llx, actuator %#llx, checksum %#x\n",
		 version, actuator, expected);
	return lc898124ep1_command(ois, LC898124EP1_REG_CENTER, 3);
}

static int lc898124ep1_load_firmware(struct lc898124ep1 *ois)
{
	const struct firmware *pm, *dm;
	char name[64];
	int ret;

	snprintf(name, sizeof(name), LC898124EP1_FW_PREFIX "%u.pm", ois->firmware_variant);
	ret = request_firmware(&pm, name, ois->sd.dev);
	if (ret)
		return ret;
	snprintf(name, sizeof(name), LC898124EP1_FW_PREFIX "%u.dm", ois->firmware_variant);
	ret = request_firmware(&dm, name, ois->sd.dev);
	if (ret)
		goto release_pm;

	if (pm->size != 864 * 5 || dm->size != 0x394 * 6 / 4)
		ret = -EINVAL;
	else
		ret = lc898124ep1_download(ois, pm, dm);
	release_firmware(dm);
release_pm:
	release_firmware(pm);
	return ret;
}

static int lc898124ep1_set_stabilization(struct lc898124ep1 *ois, bool enable)
{
	int ret;

	ret = lc898124ep1_command(ois, LC898124EP1_REG_MODE, 0);
	if (ret)
		return ret;

	return lc898124ep1_command(ois, LC898124EP1_REG_ENABLE, enable);
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

	ret = lc898124ep1_load_firmware(ois);
	if (!ret)
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
	int ret;

	ret = lc898124ep1_set_stabilization(ois, false);
	if (!ret)
		ret = lc898124ep1_command(ois, LC898124EP1_REG_CENTER, 0);
	if (ret)
		dev_warn(dev, "Failed to stop DSP before power off: %d\n", ret);
	usleep_range(2000, 3000);

	return regulator_bulk_disable(ARRAY_SIZE(ois->supplies), ois->supplies);
}

static int lc898124ep1_probe(struct i2c_client *client)
{
	struct lc898124ep1 *ois;
	unsigned int i;
	u8 vcm_version;
	int ret;

	ois = devm_kzalloc(&client->dev, sizeof(*ois), GFP_KERNEL);
	if (!ois)
		return -ENOMEM;

	ret = nvmem_cell_read_u8(&client->dev, "vcm-version", &vcm_version);
	if (ret)
		return dev_err_probe(&client->dev, ret, "Failed to read VCM version\n");
	if (vcm_version != 2 && vcm_version != 3)
		return dev_err_probe(&client->dev, -EINVAL,
				     "Unsupported VCM version: %#x\n", vcm_version);
	ois->firmware_variant = vcm_version - 1;

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
MODULE_FIRMWARE(LC898124EP1_FW_PREFIX "1.pm");
MODULE_FIRMWARE(LC898124EP1_FW_PREFIX "1.dm");
MODULE_FIRMWARE(LC898124EP1_FW_PREFIX "2.pm");
MODULE_FIRMWARE(LC898124EP1_FW_PREFIX "2.dm");
