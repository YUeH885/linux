// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "qcom_pm8150b_policy.h"

static void pm8150b_temperature_jumps(struct kunit *test)
{
	int zone;

	for (zone = PM8150B_TEMP_COLD; zone <= PM8150B_TEMP_HOT; zone++) {
		KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(zone, -10), PM8150B_TEMP_COLD);
		KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(zone, 560), PM8150B_TEMP_HOT);
		KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(zone, 50), PM8150B_TEMP_COOL);
		KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(zone, 250), PM8150B_TEMP_NORMAL);
		KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(zone, 500), PM8150B_TEMP_WARM);
	}
}

static void pm8150b_temperature_hysteresis(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_COLD, 29), PM8150B_TEMP_COLD);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_COLD, 30), PM8150B_TEMP_COOL);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_COOL, 119), PM8150B_TEMP_COOL);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_COOL, 120), PM8150B_TEMP_NORMAL);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_WARM, 430), PM8150B_TEMP_WARM);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_WARM, 429), PM8150B_TEMP_NORMAL);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_HOT, 520), PM8150B_TEMP_HOT);
	KUNIT_EXPECT_EQ(test, pm8150b_next_temp_zone(PM8150B_TEMP_HOT, 519), PM8150B_TEMP_WARM);
}

static void pm8150b_parallel_budget(struct kunit *test)
{
	int total, share;

	KUNIT_EXPECT_EQ(test, pm8150b_parallel_fcc(3000000, 3000000, 3000000,
			9000000, 4430000, 70), 2100000);
	/* A stale settled value must not bypass a new lower input limit. */
	KUNIT_EXPECT_EQ(test, pm8150b_parallel_fcc(3000000, 3000000, 100000,
			5000000, 4430000, 70), 50000);
	KUNIT_EXPECT_EQ(test, pm8150b_parallel_fcc(3000000, 0, 3000000,
			5000000, 4430000, 70), 0);
	KUNIT_EXPECT_EQ(test, pm8150b_parallel_fcc(3000000, 3000000, 3000000,
			0, 4430000, 70), 0);
	for (total = 50000; total <= 3000000; total += 50000) {
		share = pm8150b_parallel_fcc(total, 1500000, 1500000,
					    5000000, 4430000, 70);
		KUNIT_EXPECT_GE(test, share, 0);
		KUNIT_EXPECT_LE(test, share, total);
		KUNIT_EXPECT_EQ(test, share % 50000, 0);
	}
}

static void pm8150b_typec_current_priority(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, pm8150b_use_gadget_limit(false, 0));
	KUNIT_EXPECT_TRUE(test, pm8150b_use_gadget_limit(true, 500000));
	KUNIT_EXPECT_TRUE(test, pm8150b_use_gadget_limit(true, 900000));
	KUNIT_EXPECT_FALSE(test, pm8150b_use_gadget_limit(true, 1500000));
	KUNIT_EXPECT_FALSE(test, pm8150b_use_gadget_limit(true, 3000000));
}

static struct kunit_case pm8150b_policy_cases[] = {
	KUNIT_CASE(pm8150b_typec_current_priority),
	KUNIT_CASE(pm8150b_temperature_jumps),
	KUNIT_CASE(pm8150b_temperature_hysteresis),
	KUNIT_CASE(pm8150b_parallel_budget),
	{}
};

static struct kunit_suite pm8150b_policy_suite = {
	.name = "qcom-pm8150b-policy",
	.test_cases = pm8150b_policy_cases,
};
kunit_test_suite(pm8150b_policy_suite);

MODULE_LICENSE("GPL");
