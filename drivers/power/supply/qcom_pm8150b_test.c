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

static void pm8150b_board_temperature_model(struct kunit *test)
{
	const struct pm8150b_board_thermal_profile *profile = &pm8150b_alphaplus_thermal;

	KUNIT_EXPECT_EQ(test, pm8150b_board_temperature(profile, 40000, 35000,
							true, true), 38600);
	KUNIT_EXPECT_EQ(test, pm8150b_board_temperature(profile, 40000, 35000,
							true, false), 34600);
	KUNIT_EXPECT_EQ(test, pm8150b_board_temperature(profile, 40000, 35000,
							false, true), 34600);
	KUNIT_EXPECT_EQ(test, pm8150b_board_temperature(profile, 25000, 25000,
							false, false), 24000);
	KUNIT_EXPECT_EQ(test, pm8150b_board_temperature(profile, -10000, -5000,
							false, false), -3600);
}

static void pm8150b_board_thresholds(struct kunit *test)
{
	const struct pm8150b_board_thermal_profile *profile = &pm8150b_alphaplus_thermal;
	static const struct {
		int temperature_mc;
		int fcc_ua;
	} cases[] = {
		{ 9999, 0 }, { 10000, 2600000 },
		{ 38499, 2600000 }, { 38500, 1500000 },
		{ 38999, 1500000 }, { 39000, 700000 },
		{ 39999, 700000 }, { 40000, 500000 },
		{ 42999, 500000 }, { 43000, 300000 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int level = 0;
		int fcc_ua;

		fcc_ua = pm8150b_board_fcc(profile, &level, cases[i].temperature_mc);
		KUNIT_EXPECT_EQ(test, fcc_ua, cases[i].fcc_ua);
	}
}

static void pm8150b_board_hysteresis(struct kunit *test)
{
	const struct pm8150b_board_thermal_profile *profile = &pm8150b_alphaplus_thermal;
	static const struct {
		unsigned int level;
		int clear_mc;
		int fcc_ua;
		int recovered_fcc_ua;
	} cases[] = {
		{ 1, 9000, 2600000, 0 },
		{ 2, 37500, 1500000, 2600000 },
		{ 3, 38000, 700000, 1500000 },
		{ 4, 39000, 500000, 700000 },
		{ 5, 42000, 300000, 500000 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int level = cases[i].level;
		int fcc_ua;

		fcc_ua = pm8150b_board_fcc(profile, &level, cases[i].clear_mc);
		KUNIT_EXPECT_EQ(test, fcc_ua, cases[i].fcc_ua);
		KUNIT_EXPECT_EQ(test, level, cases[i].level);
		fcc_ua = pm8150b_board_fcc(profile, &level, cases[i].clear_mc - 1);
		KUNIT_EXPECT_EQ(test, fcc_ua, cases[i].recovered_fcc_ua);
		KUNIT_EXPECT_EQ(test, level, cases[i].level - 1);
	}
}

static void pm8150b_board_temperature_jumps(struct kunit *test)
{
	unsigned int initial_level;

	for (initial_level = 0; initial_level <= 5; initial_level++) {
		unsigned int level = initial_level;

		KUNIT_EXPECT_EQ(test, pm8150b_board_fcc(&pm8150b_alphaplus_thermal,
							&level, 45000), 300000);
		KUNIT_EXPECT_EQ(test, level, 5);
		KUNIT_EXPECT_EQ(test, pm8150b_board_fcc(&pm8150b_alphaplus_thermal,
							&level, 36000), 2600000);
		KUNIT_EXPECT_EQ(test, level, 1);
		KUNIT_EXPECT_EQ(test, pm8150b_board_fcc(&pm8150b_alphaplus_thermal,
							&level, 8000), 0);
		KUNIT_EXPECT_EQ(test, level, 0);
	}
}

static void pm8150b_board_display_transition(struct kunit *test)
{
	const struct pm8150b_board_thermal_profile *profile = &pm8150b_alphaplus_thermal;
	unsigned int level = 0;
	int temperature_mc;

	temperature_mc = pm8150b_board_temperature(profile, 40000, 35000, true, true);
	KUNIT_EXPECT_EQ(test, pm8150b_board_fcc(profile, &level, temperature_mc), 1500000);
	temperature_mc = pm8150b_board_temperature(profile, 40000, 35000, true, false);
	KUNIT_EXPECT_EQ(test, pm8150b_board_fcc(profile, &level, temperature_mc), 2600000);
	temperature_mc = pm8150b_board_temperature(profile, 40000, 35000, true, true);
	KUNIT_EXPECT_EQ(test, pm8150b_board_fcc(profile, &level, temperature_mc), 1500000);
}

static struct kunit_case pm8150b_policy_cases[] = {
	KUNIT_CASE(pm8150b_board_temperature_model),
	KUNIT_CASE(pm8150b_board_thresholds),
	KUNIT_CASE(pm8150b_board_hysteresis),
	KUNIT_CASE(pm8150b_board_temperature_jumps),
	KUNIT_CASE(pm8150b_board_display_transition),
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
