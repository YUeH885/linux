// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/iio/iio.h>

struct pm8150b_pm_test {
	struct pm8150b_charger chip;
	struct power_supply psy;
	struct device dev;
	unsigned int runs;
	int applied_limit;
	int applied_board_fcc;
	int board_read_error;
	struct iio_dev *indio_dev;
	struct iio_channel channels[2];
	struct backlight_device backlight;
	int temperatures[2];
	int errors[2];
	unsigned int reads[2];
};

static const struct iio_chan_spec pm8150b_test_channels[] = {
	{ .type = IIO_TEMP, .channel = 0,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) },
	{ .type = IIO_TEMP, .channel = 1,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) },
};

static int pm8150b_test_read_raw(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 int *val, int *val2, long mask)
{
	struct pm8150b_pm_test *ctx = *(struct pm8150b_pm_test **)iio_priv(indio_dev);
	unsigned int channel = chan->channel;

	ctx->reads[channel]++;
	if (ctx->errors[channel])
		return ctx->errors[channel];
	*val = ctx->temperatures[channel];

	return IIO_VAL_INT;
}

static const struct iio_info pm8150b_test_iio_info = {
	.read_raw = pm8150b_test_read_raw,
};

static void pm8150b_test_policy_work(struct work_struct *work)
{
	struct pm8150b_pm_test *ctx = container_of(work, struct pm8150b_pm_test,
						 chip.policy_work.work);

	ctx->runs++;
	ctx->applied_limit = ctx->chip.gadget_current_limit_ua;
	if (ctx->chip.board_thermal) {
		struct pm8150b_policy_snapshot snapshot = {
			.present = true,
			.online = true,
			.battery_temp_decic = 250,
			.battery_voltage_uv = 4100000,
			.source_current_max_ua = 1500000,
			.phase = PM8150B_FULLON_CHARGE,
		};
		struct pm8150b_policy_target target;
		int ret;

		ret = pm8150b_read_board_temperature(&ctx->chip, true,
						     &snapshot.board_temperature_mc);
		ctx->board_read_error = ret;
		if (!ctx->board_read_error) {
			pm8150b_calculate_policy(&ctx->chip, &snapshot, &target);
			ctx->applied_board_fcc = target.fcc_ua;
		}
	}
}

static int pm8150b_pm_test_init(struct kunit *test)
{
	struct pm8150b_pm_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	mutex_init(&ctx->chip.usb_lock);
	mutex_init(&ctx->chip.adc_lock);
	INIT_DELAYED_WORK(&ctx->chip.policy_work, pm8150b_test_policy_work);
	ctx->psy.drv_data = &ctx->chip;
	dev_set_drvdata(&ctx->dev, &ctx->chip);
	ctx->indio_dev = iio_device_alloc(NULL, sizeof(ctx));
	if (!ctx->indio_dev)
		return -ENOMEM;
	*(struct pm8150b_pm_test **)iio_priv(ctx->indio_dev) = ctx;
	ctx->indio_dev->info = &pm8150b_test_iio_info;
	ctx->channels[0].indio_dev = ctx->indio_dev;
	ctx->channels[0].channel = &pm8150b_test_channels[0];
	ctx->channels[1].indio_dev = ctx->indio_dev;
	ctx->channels[1].channel = &pm8150b_test_channels[1];
	ctx->chip.xo_therm = &ctx->channels[0];
	ctx->chip.skin_therm = &ctx->channels[1];
	ctx->chip.backlight = &ctx->backlight;
	ctx->chip.profile_fcc_ua = 3000000;
	ctx->chip.profile_fv_uv = 4430000;
	ctx->chip.charge_full_design_uah = 3400000;
	ctx->chip.typec_online = true;
	ctx->backlight.props.brightness = 100;
	ctx->temperatures[0] = 40000;
	ctx->temperatures[1] = 35000;
	return 0;
}

static void pm8150b_pm_test_exit(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;

	cancel_delayed_work_sync(&ctx->chip.policy_work);
	iio_device_free(ctx->indio_dev);
}

static void pm8150b_active_limit(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;
	union power_supply_propval val = { .intval = 500000 };

	KUNIT_ASSERT_EQ(test, pm8150b_usb_set_property(&ctx->psy,
			POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val), 0);
	flush_delayed_work(&ctx->chip.policy_work);
	KUNIT_EXPECT_EQ(test, ctx->runs, 1);
	KUNIT_EXPECT_EQ(test, ctx->applied_limit, 500000);
}

static void pm8150b_deferred_limits(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;
	union power_supply_propval val = { .intval = 0 };

	mod_delayed_work(system_dfl_wq, &ctx->chip.policy_work, 60 * HZ);
	KUNIT_ASSERT_EQ(test, pm8150b_prepare(&ctx->dev), 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&ctx->chip.policy_work));
	KUNIT_ASSERT_EQ(test, pm8150b_usb_set_property(&ctx->psy,
			POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val), 0);
	val.intval = 900000;
	KUNIT_ASSERT_EQ(test, pm8150b_usb_set_property(&ctx->psy,
			POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val), 0);
	flush_delayed_work(&ctx->chip.policy_work);
	KUNIT_EXPECT_EQ(test, ctx->runs, 0);
	KUNIT_EXPECT_TRUE(test, ctx->chip.gadget_current_limit_valid);
	KUNIT_EXPECT_EQ(test, ctx->chip.gadget_current_limit_ua, 900000);

	pm8150b_complete(&ctx->dev);
	KUNIT_EXPECT_FALSE(test, ctx->chip.suspended);
	KUNIT_EXPECT_EQ(test, ctx->runs, 1);
	KUNIT_EXPECT_EQ(test, ctx->applied_limit, 900000);
}

static void pm8150b_aborted_prepare(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;
	union power_supply_propval val = { .intval = 100000 };

	KUNIT_ASSERT_EQ(test, pm8150b_prepare(&ctx->dev), 0);
	/* PM calls complete even if another device aborts preparation. */
	pm8150b_complete(&ctx->dev);
	KUNIT_ASSERT_EQ(test, pm8150b_usb_set_property(&ctx->psy,
			POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val), 0);
	flush_delayed_work(&ctx->chip.policy_work);
	KUNIT_EXPECT_EQ(test, ctx->runs, 2);
	KUNIT_EXPECT_EQ(test, ctx->applied_limit, 100000);
}

static void pm8150b_shutdown_stays_stopped(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;
	union power_supply_propval val = { .intval = 100000 };

	KUNIT_ASSERT_EQ(test, pm8150b_prepare(&ctx->dev), 0);
	ctx->chip.shutting_down = true;
	KUNIT_EXPECT_EQ(test, pm8150b_usb_set_property(&ctx->psy,
			POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val), -ESHUTDOWN);
	pm8150b_complete(&ctx->dev);
	KUNIT_EXPECT_EQ(test, ctx->runs, 0);
}

static void pm8150b_board_policy_limits(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;
	struct pm8150b_policy_snapshot snapshot = {
		.present = true,
		.online = true,
		.battery_temp_decic = 250,
		.battery_voltage_uv = 4100000,
		.source_current_max_ua = 1500000,
		.board_temperature_mc = 39000,
		.phase = PM8150B_FULLON_CHARGE,
	};
	struct pm8150b_policy_target target;
	int parallel_fcc_ua;

	ctx->chip.board_thermal = &pm8150b_alphaplus_thermal;
	pm8150b_calculate_policy(&ctx->chip, &snapshot, &target);
	KUNIT_EXPECT_EQ(test, target.fcc_ua, 700000);
	KUNIT_EXPECT_FALSE(test, target.disable_charging);
	parallel_fcc_ua = pm8150b_parallel_fcc(target.fcc_ua, 1500000, 1500000,
					       5000000, target.fv_uv, 70);
	KUNIT_EXPECT_LT(test, parallel_fcc_ua, PM8150B_PARALLEL_MIN_FCC_UA);

	snapshot.board_temperature_mc = 45000;
	ctx->chip.bvp_fcc_ua = 1000000;
	pm8150b_calculate_policy(&ctx->chip, &snapshot, &target);
	KUNIT_EXPECT_EQ(test, target.fcc_ua, 300000);
	snapshot.board_temperature_mc = 36000;
	pm8150b_calculate_policy(&ctx->chip, &snapshot, &target);
	KUNIT_EXPECT_EQ(test, target.fcc_ua, 1000000);

	ctx->chip.bvp_fcc_ua = 0;
	snapshot.battery_temp_decic = 500;
	pm8150b_calculate_policy(&ctx->chip, &snapshot, &target);
	KUNIT_EXPECT_EQ(test, target.fcc_ua, 1700000);
	KUNIT_EXPECT_EQ(test, target.fv_uv, 4000000);
	snapshot.present = false;
	pm8150b_calculate_policy(&ctx->chip, &snapshot, &target);
	KUNIT_EXPECT_TRUE(test, target.disable_charging);
	KUNIT_EXPECT_EQ(test, ctx->chip.board_thermal_level, 0);
}

static void pm8150b_board_resume_samples(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;

	ctx->chip.board_thermal = &pm8150b_alphaplus_thermal;
	mod_delayed_work(system_dfl_wq, &ctx->chip.policy_work, 0);
	flush_delayed_work(&ctx->chip.policy_work);
	KUNIT_ASSERT_EQ(test, ctx->board_read_error, 0);
	KUNIT_EXPECT_EQ(test, ctx->applied_board_fcc, 1500000);
	KUNIT_ASSERT_EQ(test, pm8150b_prepare(&ctx->dev), 0);
	ctx->temperatures[0] = 45000;
	ctx->temperatures[1] = 45000;
	pm8150b_complete(&ctx->dev);
	KUNIT_ASSERT_EQ(test, ctx->board_read_error, 0);
	KUNIT_EXPECT_EQ(test, ctx->runs, 2);
	KUNIT_EXPECT_EQ(test, ctx->applied_board_fcc, 300000);

	KUNIT_ASSERT_EQ(test, pm8150b_prepare(&ctx->dev), 0);
	ctx->temperatures[0] = 40000;
	ctx->temperatures[1] = 35000;
	ctx->backlight.props.state = BL_CORE_FBBLANK;
	pm8150b_complete(&ctx->dev);
	KUNIT_EXPECT_EQ(test, ctx->runs, 3);
	KUNIT_EXPECT_EQ(test, ctx->applied_board_fcc, 2600000);
}

static void pm8150b_board_sensor_errors(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;
	int temperature_mc = -777;

	ctx->chip.board_thermal = &pm8150b_alphaplus_thermal;
	ctx->errors[0] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, pm8150b_read_board_temperature(&ctx->chip, true,
							     &temperature_mc), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->reads[0], 1);
	KUNIT_EXPECT_EQ(test, ctx->reads[1], 0);
	KUNIT_EXPECT_EQ(test, temperature_mc, -777);
	ctx->errors[0] = 0;
	ctx->errors[1] = -EIO;
	KUNIT_EXPECT_EQ(test, pm8150b_read_board_temperature(&ctx->chip, true,
							     &temperature_mc), -EIO);
	KUNIT_EXPECT_EQ(test, temperature_mc, -777);
}

static struct kunit_case pm8150b_pm_cases[] = {
	KUNIT_CASE(pm8150b_board_policy_limits),
	KUNIT_CASE(pm8150b_board_resume_samples),
	KUNIT_CASE(pm8150b_board_sensor_errors),
	KUNIT_CASE(pm8150b_active_limit),
	KUNIT_CASE(pm8150b_deferred_limits),
	KUNIT_CASE(pm8150b_aborted_prepare),
	KUNIT_CASE(pm8150b_shutdown_stays_stopped),
	{}
};

static struct kunit_suite pm8150b_pm_suite = {
	.name = "qcom-pm8150b-pm",
	.init = pm8150b_pm_test_init,
	.exit = pm8150b_pm_test_exit,
	.test_cases = pm8150b_pm_cases,
};
kunit_test_suite(pm8150b_pm_suite);
