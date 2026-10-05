// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

struct pm8150b_pm_test {
	struct pm8150b_charger chip;
	struct power_supply psy;
	struct device dev;
	unsigned int runs;
	int applied_limit;
};

static void pm8150b_test_policy_work(struct work_struct *work)
{
	struct pm8150b_pm_test *ctx = container_of(work, struct pm8150b_pm_test,
						 chip.policy_work.work);

	ctx->runs++;
	ctx->applied_limit = ctx->chip.gadget_current_limit_ua;
}

static int pm8150b_pm_test_init(struct kunit *test)
{
	struct pm8150b_pm_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	mutex_init(&ctx->chip.usb_lock);
	INIT_DELAYED_WORK(&ctx->chip.policy_work, pm8150b_test_policy_work);
	ctx->psy.drv_data = &ctx->chip;
	dev_set_drvdata(&ctx->dev, &ctx->chip);
	return 0;
}

static void pm8150b_pm_test_exit(struct kunit *test)
{
	struct pm8150b_pm_test *ctx = test->priv;

	cancel_delayed_work_sync(&ctx->chip.policy_work);
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

static struct kunit_case pm8150b_pm_cases[] = {
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
