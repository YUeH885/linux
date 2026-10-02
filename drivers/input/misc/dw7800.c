// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dongwoon Anatech DW7800 Haptics Driver
 *
 * Copyright (c) 2018 Dongwoon Anatech Co., Ltd.
 * Copyright (c) 2026 Yuzu <yuzu23234@gmail.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define DW7800_REG_INFO		0x00
#define DW7800_REG_VERSION	0x01
#define DW7800_REG_STATUS	0x02
#define DW7800_REG_FIFOFULL	0x03
#define DW7800_REG_DATA		0x04
#define DW7800_REG_SWRESET	0x05
#define DW7800_REG_TIMING	0x06
#define DW7800_REG_LDO		0x07
#define DW7800_REG_HWRESET	0x08
#define DW7800_REG_PKTSIZE	0x09

#define DW7800_SWRESET_CMD	0x01
#define DW7800_HWRESET_ENABLE	0x00

/* 10ms timeout, 8kHz sample frequency */
#define DW7800_TIMING_8KHZ_10MS	((2 << 4) | 0)
/* 2.8V LDO output */
#define DW7800_LDO_2P8V		8

#define DW7800_FIFO_CAPACITY	86
#define DW7800_FIFO_WATERMARK	60
#define DW7800_MAX_CHUNK	32

/* Normalized 48-sample sine wave cycle (~167 Hz at 8 kHz sample rate) */
static const s8 dw7800_sine_table[48] = {
	0, 16, 32, 48, 63, 77, 89, 100, 109, 116, 121, 124,
	125, 124, 121, 116, 109, 100, 89, 77, 63, 48, 32, 16,
	0, -16, -32, -48, -63, -77, -89, -100, -109, -116, -121, -124,
	-125, -124, -121, -116, -109, -100, -89, -77, -63, -48, -32, -16,
};

struct dw7800_data {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *input_dev;
	struct work_struct work;
	spinlock_t lock;
	u16 magnitude;
	unsigned int duration_ms;
	bool running;
};

static const struct regmap_config dw7800_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = DW7800_REG_PKTSIZE,
	.cache_type = REGCACHE_NONE,
};

static int dw7800_init_hw(struct dw7800_data *dw)
{
	int ret;

	fsleep(5000);
	ret = regmap_write(dw->regmap, DW7800_REG_SWRESET, DW7800_SWRESET_CMD);
	if (ret)
		return ret;

	fsleep(1000);
	ret = regmap_write(dw->regmap, DW7800_REG_TIMING, DW7800_TIMING_8KHZ_10MS);
	if (ret)
		return ret;

	fsleep(1000);
	ret = regmap_write(dw->regmap, DW7800_REG_LDO, DW7800_LDO_2P8V);
	if (ret)
		return ret;

	fsleep(1000);
	ret = regmap_write(dw->regmap, DW7800_REG_HWRESET, DW7800_HWRESET_ENABLE);
	if (ret)
		return ret;

	fsleep(1000);
	return 0;
}

static void dw7800_worker(struct work_struct *work)
{
	struct dw7800_data *dw = container_of(work, struct dw7800_data, work);
	ktime_t end_time;
	size_t phase = 0;
	u8 buf[DW7800_MAX_CHUNK];
	unsigned int fifo_val;
	unsigned int duration_ms;
	u16 mag;
	int ret, i;

	spin_lock_irq(&dw->lock);
	if (!dw->running || !dw->magnitude) {
		spin_unlock_irq(&dw->lock);
		return;
	}
	mag = dw->magnitude;
	duration_ms = dw->duration_ms;
	spin_unlock_irq(&dw->lock);

	end_time = ktime_add_ms(ktime_get(), duration_ms);

	while (ktime_before(ktime_get(), end_time)) {
		spin_lock_irq(&dw->lock);
		if (!dw->running) {
			spin_unlock_irq(&dw->lock);
			break;
		}
		mag = dw->magnitude;
		spin_unlock_irq(&dw->lock);

		ret = regmap_read(dw->regmap, DW7800_REG_FIFOFULL, &fifo_val);
		if (ret) {
			dev_err(dw->dev, "failed to read FIFO fullness: %d\n", ret);
			break;
		}

		if (fifo_val < DW7800_FIFO_WATERMARK) {
			for (i = 0; i < DW7800_MAX_CHUNK; i++) {
				s32 sample = dw7800_sine_table[phase];

				sample = (sample * mag) / U16_MAX;
				buf[i] = (u8)sample;
				phase = (phase + 1) % ARRAY_SIZE(dw7800_sine_table);
			}

			ret = regmap_raw_write(dw->regmap, DW7800_REG_DATA,
					       buf, DW7800_MAX_CHUNK);
			if (ret) {
				dev_err(dw->dev, "failed to stream haptic data: %d\n", ret);
				break;
			}
		}

		usleep_range(2000, 3000);
	}

	spin_lock_irq(&dw->lock);
	dw->running = false;
	spin_unlock_irq(&dw->lock);
}

static int dw7800_haptics_play(struct input_dev *input, void *data,
			       struct ff_effect *effect)
{
	struct dw7800_data *dw = input_get_drvdata(input);
	unsigned long flags;
	u16 mag;

	mag = max(effect->u.rumble.strong_magnitude,
		  effect->u.rumble.weak_magnitude);

	spin_lock_irqsave(&dw->lock, flags);
	if (!mag) {
		dw->running = false;
		spin_unlock_irqrestore(&dw->lock, flags);
		return 0;
	}

	dw->magnitude = mag;
	dw->duration_ms = effect->replay.length ? effect->replay.length : 50;
	if (dw->duration_ms > 5000)
		dw->duration_ms = 5000;

	dw->running = true;
	spin_unlock_irqrestore(&dw->lock, flags);

	schedule_work(&dw->work);

	return 0;
}

static void dw7800_close(struct input_dev *input)
{
	struct dw7800_data *dw = input_get_drvdata(input);
	unsigned long flags;

	spin_lock_irqsave(&dw->lock, flags);
	dw->running = false;
	spin_unlock_irqrestore(&dw->lock, flags);

	cancel_work_sync(&dw->work);
}

static int dw7800_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct dw7800_data *dw;
	unsigned int version;
	int ret;

	dw = devm_kzalloc(dev, sizeof(*dw), GFP_KERNEL);
	if (!dw)
		return -ENOMEM;

	dw->dev = dev;
	spin_lock_init(&dw->lock);
	i2c_set_clientdata(client, dw);
	INIT_WORK(&dw->work, dw7800_worker);

	dw->regmap = devm_regmap_init_i2c(client, &dw7800_regmap_config);
	if (IS_ERR(dw->regmap))
		return dev_err_probe(dev, PTR_ERR(dw->regmap),
				      "failed to initialize register map\n");

	ret = dw7800_init_hw(dw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize DW7800\n");

	ret = regmap_read(dw->regmap, DW7800_REG_VERSION, &version);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read version register\n");

	dev_info(dev, "DW7800 haptic controller version: 0x%02x\n", version);

	dw->input_dev = devm_input_allocate_device(dev);
	if (!dw->input_dev)
		return -ENOMEM;

	dw->input_dev->name = "dw7800:haptics";
	dw->input_dev->close = dw7800_close;
	input_set_drvdata(dw->input_dev, dw);
	input_set_capability(dw->input_dev, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(dw->input_dev, NULL, dw7800_haptics_play);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create FF device\n");

	return input_register_device(dw->input_dev);
}

static int dw7800_suspend(struct device *dev)
{
	struct dw7800_data *dw = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&dw->lock, flags);
	dw->running = false;
	spin_unlock_irqrestore(&dw->lock, flags);

	cancel_work_sync(&dw->work);

	return 0;
}

static int dw7800_resume(struct device *dev)
{
	struct dw7800_data *dw = dev_get_drvdata(dev);

	return dw7800_init_hw(dw);
}

static DEFINE_SIMPLE_DEV_PM_OPS(dw7800_pm_ops, dw7800_suspend, dw7800_resume);

static const struct i2c_device_id dw7800_i2c_id[] = {
	{ "dw7800", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, dw7800_i2c_id);

static const struct of_device_id dw7800_of_match[] = {
	{ .compatible = "dongwoon,dw7800" },
	{ }
};
MODULE_DEVICE_TABLE(of, dw7800_of_match);

static struct i2c_driver dw7800_driver = {
	.driver = {
		.name = "dw7800",
		.pm = pm_sleep_ptr(&dw7800_pm_ops),
		.of_match_table = dw7800_of_match,
	},
	.probe = dw7800_probe,
	.id_table = dw7800_i2c_id,
};
module_i2c_driver(dw7800_driver);

MODULE_DESCRIPTION("Dongwoon Anatech DW7800 haptics driver");
MODULE_AUTHOR("Yuzu <yuzu23234@gmail.com>");
MODULE_LICENSE("GPL");
