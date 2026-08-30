// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include <drm/drm_panel.h>

#define SW42000_MAX_TOUCHES		10

#define SW42000_REG_STATUS		0x600
#define SW42000_REG_VERSION		0x642
#define SW42000_REG_INTERNAL_LDO	0x006
#define SW42000_REG_INTERNAL_CLOCK	0xfe1
#define SW42000_REG_SERIAL_SPI_ENABLE	0xfe4
#define SW42000_REG_DEVICE_CONTROL	0xc00
#define SW42000_REG_INTERRUPT_CONTROL	0xc01
#define SW42000_REG_DRIVING_CONTROL	0xc03

#define SW42000_DRIVING_STOP		0x002
#define SW42000_DRIVING_NORMAL		0x311

#define SW42000_INTERRUPT_TYPE_MASK	GENMASK(19, 16)
#define SW42000_INTERRUPT_TYPE_REPORT	5
#define SW42000_WAKEUP_TYPE_MASK		GENMASK(7, 0)
#define SW42000_TOUCH_COUNT_MASK		GENMASK(12, 8)

#define SW42000_EVENT_DOWN		1
#define SW42000_EVENT_MOVE		2
#define SW42000_EVENT_UP			3

enum sw42000_supply {
	SW42000_SUPPLY_VCL,
	SW42000_SUPPLY_VDD,
	SW42000_NUM_SUPPLIES,
};

struct sw42000_report {
	__le32 ic_status;
	__le32 tc_status;
	__le32 info;
	__le32 points[SW42000_MAX_TOUCHES][3];
} __packed;

static_assert(sizeof(struct sw42000_report) == 132);

struct sw42000 {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[SW42000_NUM_SUPPLIES];
	struct touchscreen_properties prop;
	struct drm_panel_follower panel_follower;
	/* 串行化面板和系统电源状态切换。 */
	struct mutex lock;
	bool powered;
	bool active;
	bool panel_prepared;
	bool suspended;
};

static int sw42000_read(struct sw42000 *ts, u16 reg, void *data, size_t len)
{
	u8 address[] = {
		(len > sizeof(u32) ? 0x20 : 0) | ((reg >> 8) & 0x0f),
		reg & 0xff,
	};
	struct i2c_msg messages[] = {
		{
			.addr = ts->client->addr,
			.len = sizeof(address),
			.buf = address,
		},
		{
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = data,
		},
	};
	int ret;

	ret = i2c_transfer(ts->client->adapter, messages, ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int sw42000_write_u32(struct sw42000 *ts, u16 reg, u32 value)
{
	u8 data[6] = {
		0x40 | ((reg >> 8) & 0x0f),
		reg & 0xff,
	};
	int ret;

	put_unaligned_le32(value, data + 2);
	ret = i2c_master_send(ts->client, data, sizeof(data));
	if (ret == sizeof(data))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int sw42000_start(struct sw42000 *ts)
{
	u8 version[4];
	bool enabled_supplies = false;
	int disable_error;
	int error;

	if (!ts->powered) {
		error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies),
					      ts->supplies);
		if (error)
			return error;
		ts->powered = true;
		enabled_supplies = true;
		usleep_range(10000, 11000);
	}

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(100);

	error = sw42000_read(ts, SW42000_REG_VERSION, version, sizeof(version));
	if (error)
		goto disable;

	if (version[2] != 10 || (version[3] & 0x0f) != 4) {
		dev_err(&ts->client->dev,
			"无法识别控制器：chip=%u protocol=%u\n",
			version[2], version[3] & 0x0f);
		error = -ENODEV;
		goto disable;
	}

	error = sw42000_write_u32(ts, SW42000_REG_SERIAL_SPI_ENABLE, 0);
	if (error)
		goto disable;

	error = sw42000_write_u32(ts, SW42000_REG_DEVICE_CONTROL, 1);
	if (error)
		goto disable;

	error = sw42000_write_u32(ts, SW42000_REG_INTERRUPT_CONTROL, 1);
	if (error)
		goto disable;

	error = sw42000_write_u32(ts, SW42000_REG_DRIVING_CONTROL,
				  SW42000_DRIVING_STOP);
	if (error)
		goto disable;

	msleep(30);
	error = sw42000_write_u32(ts, SW42000_REG_DRIVING_CONTROL,
				  SW42000_DRIVING_NORMAL);
	if (error)
		goto disable;

	dev_dbg(&ts->client->dev, "SW42000 v%u.%02u 已启动\n",
		version[0] >> 4, version[1]);
	return 0;

disable:
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	if (enabled_supplies) {
		disable_error = regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
		if (disable_error)
			dev_warn(&ts->client->dev,
				 "关闭触控电源失败：%d\n", disable_error);
		else
			ts->powered = false;
	}
	return error;
}

static void sw42000_release_touches(struct sw42000 *ts)
{
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static int sw42000_deep_sleep(struct sw42000 *ts)
{
	int error;

	if (!ts->active)
		return 0;

	disable_irq(ts->client->irq);
	ts->active = false;
	sw42000_release_touches(ts);

	error = sw42000_write_u32(ts, SW42000_REG_DRIVING_CONTROL,
				  SW42000_DRIVING_STOP);
	if (error)
		return error;

	msleep(30);
	error = sw42000_write_u32(ts, SW42000_REG_INTERNAL_LDO, 0);
	if (error)
		return error;

	return sw42000_write_u32(ts, SW42000_REG_INTERNAL_CLOCK, 0);
}

static int sw42000_activate(struct sw42000 *ts)
{
	int error;

	if (ts->active)
		return 0;

	error = sw42000_start(ts);
	if (error)
		return error;

	enable_irq(ts->client->irq);
	ts->active = true;
	return 0;
}

static int sw42000_stop(struct sw42000 *ts)
{
	int error;

	if (!ts->powered)
		return 0;

	if (ts->active) {
		disable_irq(ts->client->irq);
		ts->active = false;
		sw42000_release_touches(ts);
		sw42000_write_u32(ts, SW42000_REG_INTERRUPT_CONTROL, 0);
		sw42000_write_u32(ts, SW42000_REG_DRIVING_CONTROL,
				  SW42000_DRIVING_STOP);
	}
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	error = regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return error;

	ts->powered = false;
	return 0;
}

static void sw42000_stop_action(void *data)
{
	struct sw42000 *ts = data;
	int error;

	error = sw42000_stop(ts);
	if (error)
		dev_warn(&ts->client->dev, "关闭触控电源失败：%d\n", error);
}

static int sw42000_panel_prepared(struct drm_panel_follower *follower)
{
	struct sw42000 *ts = container_of(follower, struct sw42000,
					  panel_follower);
	int error = 0;

	guard(mutex)(&ts->lock);
	ts->panel_prepared = true;
	if (!ts->suspended)
		error = sw42000_activate(ts);

	return error;
}

static int sw42000_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct sw42000 *ts = container_of(follower, struct sw42000,
					  panel_follower);
	int error = 0;

	guard(mutex)(&ts->lock);
	ts->panel_prepared = false;
	if (!ts->suspended)
		error = sw42000_deep_sleep(ts);

	return error;
}

static const struct drm_panel_follower_funcs sw42000_panel_follower_funcs = {
	.panel_prepared = sw42000_panel_prepared,
	.panel_unpreparing = sw42000_panel_unpreparing,
};

static void sw42000_report_touch(struct sw42000 *ts, const __le32 point[3])
{
	u32 first = le32_to_cpu(point[0]);
	u32 second = le32_to_cpu(point[1]);
	u32 third = le32_to_cpu(point[2]);
	u8 id = FIELD_GET(GENMASK(4, 0), first);
	u8 event = FIELD_GET(GENMASK(17, 16), first);
	u16 x = FIELD_GET(GENMASK(31, 18), first);
	u16 y = FIELD_GET(GENMASK(13, 0), second);
	u8 pressure = FIELD_GET(GENMASK(21, 14), second);
	u16 major = FIELD_GET(GENMASK(17, 4), third);
	u16 minor = FIELD_GET(GENMASK(31, 18), third);
	bool active;

	if (id >= SW42000_MAX_TOUCHES)
		return;

	active = event == SW42000_EVENT_DOWN || event == SW42000_EVENT_MOVE;
	if (!active && event != SW42000_EVENT_UP)
		return;

	input_mt_slot(ts->input, id);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, active);
	if (!active)
		return;

	touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
	input_report_abs(ts->input, ABS_MT_PRESSURE, pressure);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, major);
	input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, minor);
}

static irqreturn_t sw42000_irq_thread(int irq, void *data)
{
	struct sw42000 *ts = data;
	struct sw42000_report report;
	u32 status;
	u32 info;
	u8 count;
	int error;
	int i;

	error = sw42000_read(ts, SW42000_REG_STATUS, &report, sizeof(report));
	if (error) {
		dev_err_ratelimited(&ts->client->dev,
				    "读取触摸事件失败：%d\n", error);
		return IRQ_HANDLED;
	}

	status = le32_to_cpu(report.tc_status);
	if (FIELD_GET(SW42000_INTERRUPT_TYPE_MASK, status) !=
	    SW42000_INTERRUPT_TYPE_REPORT)
		return IRQ_HANDLED;

	info = le32_to_cpu(report.info);
	if (FIELD_GET(SW42000_WAKEUP_TYPE_MASK, info) != 0)
		return IRQ_HANDLED;

	count = min_t(u8, FIELD_GET(SW42000_TOUCH_COUNT_MASK, info),
		      SW42000_MAX_TOUCHES);
	for (i = 0; i < count; i++)
		sw42000_report_touch(ts, report.points[i]);

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static int sw42000_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sw42000 *ts;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);
	mutex_init(&ts->lock);
	ts->panel_follower.funcs = &sw42000_panel_follower_funcs;

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "无法获取 reset GPIO\n");

	ts->supplies[SW42000_SUPPLY_VCL].supply = "vcl";
	ts->supplies[SW42000_SUPPLY_VDD].supply = "vdd";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies),
					ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "无法获取触控电源\n");

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "LG SW42000 Touchscreen";
	ts->input->id.bustype = BUS_I2C;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 1439, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 3119, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 3119, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 3119, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);

	error = input_mt_init_slots(ts->input, SW42000_MAX_TOUCHES,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	error = devm_add_action_or_reset(dev, sw42000_stop_action, ts);
	if (error)
		return error;

	error = sw42000_start(ts);
	if (error)
		return dev_err_probe(dev, error, "控制器启动失败\n");

	error = input_register_device(ts->input);
	if (error)
		return error;

	error = devm_request_threaded_irq(dev, client->irq, NULL,
					  sw42000_irq_thread, IRQF_ONESHOT,
					  client->name, ts);
	if (error)
		return dev_err_probe(dev, error, "无法申请中断\n");
	ts->active = true;

	error = devm_drm_panel_add_follower(dev, &ts->panel_follower);
	if (error) {
		if (ts->active) {
			disable_irq(client->irq);
			ts->active = false;
		}
		return dev_err_probe(dev, error, "无法跟随显示面板\n");
	}

	guard(mutex)(&ts->lock);
	if (!ts->panel_prepared) {
		error = sw42000_deep_sleep(ts);
		if (error)
			return dev_err_probe(dev, error, "控制器休眠失败\n");
	}

	return 0;
}

static int sw42000_suspend(struct device *dev)
{
	struct sw42000 *ts = dev_get_drvdata(dev);
	int resume_error;
	int error;

	guard(mutex)(&ts->lock);
	ts->suspended = true;
	error = sw42000_stop(ts);
	if (!error)
		return 0;

	ts->suspended = false;
	if (ts->panel_prepared) {
		resume_error = sw42000_activate(ts);
		if (resume_error)
			dev_err(dev, "suspend 失败后恢复触控失败：%d\n",
				resume_error);
	}

	return error;
}

static int sw42000_resume(struct device *dev)
{
	struct sw42000 *ts = dev_get_drvdata(dev);
	int error = 0;

	guard(mutex)(&ts->lock);
	ts->suspended = false;
	if (ts->panel_prepared)
		error = sw42000_activate(ts);

	return error;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sw42000_pm_ops, sw42000_suspend,
				 sw42000_resume);

static const struct of_device_id sw42000_of_match[] = {
	{ .compatible = "lg,sw42000" },
	{ }
};
MODULE_DEVICE_TABLE(of, sw42000_of_match);

static struct i2c_driver sw42000_driver = {
	.driver = {
		.name = "lg-sw42000",
		.of_match_table = sw42000_of_match,
		.pm = pm_sleep_ptr(&sw42000_pm_ops),
	},
	.probe = sw42000_probe,
};
module_i2c_driver(sw42000_driver);

MODULE_DESCRIPTION("LG SW42000 touchscreen driver");
MODULE_LICENSE("GPL");
