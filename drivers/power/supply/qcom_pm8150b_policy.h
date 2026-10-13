/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_PM8150B_POLICY_H
#define QCOM_PM8150B_POLICY_H

#include <linux/math64.h>
#include <linux/minmax.h>

struct pm8150b_board_thermal_trip {
	int temperature_mc;
	int clear_mc;
	int fcc_ua;
};

struct pm8150b_board_thermal_profile {
	int xo_coefficient;
	int skin_coefficient;
	int offset;
	int denominator;
	int display_offset_mc;
	const struct pm8150b_board_thermal_trip *trips;
	unsigned int num_trips;
};

static const struct pm8150b_board_thermal_trip pm8150b_alphaplus_thermal_trips[] = {
	{ 10000, 9000, 2600000 },
	{ 38500, 37500, 1500000 },
	{ 39000, 38000, 700000 },
	{ 40000, 39000, 500000 },
	{ 43000, 42000, 300000 },
};

static const struct pm8150b_board_thermal_profile pm8150b_alphaplus_thermal = {
	.xo_coefficient = 410,
	.skin_coefficient = 446,
	.offset = 2640000,
	.denominator = 1000,
	.display_offset_mc = 4000,
	.trips = pm8150b_alphaplus_thermal_trips,
	.num_trips = ARRAY_SIZE(pm8150b_alphaplus_thermal_trips),
};

static inline int
pm8150b_board_temperature(const struct pm8150b_board_thermal_profile *profile,
			  int xo_mc, int skin_mc, bool wired, bool display_on)
{
	s64 temperature = (s64)xo_mc * profile->xo_coefficient +
			  (s64)skin_mc * profile->skin_coefficient + profile->offset;
	int temperature_mc = div_s64(temperature, profile->denominator);

	/* The stock VTS interface reports tenths of a degree. */
	temperature_mc = temperature_mc / 100 * 100;
	if (wired && display_on)
		temperature_mc += profile->display_offset_mc;

	return temperature_mc;
}

static inline int
pm8150b_board_fcc(const struct pm8150b_board_thermal_profile *profile,
		  unsigned int *level, int temperature_mc)
{
	while (*level < profile->num_trips &&
	       temperature_mc >= profile->trips[*level].temperature_mc)
		(*level)++;
	while (*level && temperature_mc < profile->trips[*level - 1].clear_mc)
		(*level)--;

	return *level ? profile->trips[*level - 1].fcc_ua : 0;
}

enum pm8150b_temp_zone {
	PM8150B_TEMP_COLD,
	PM8150B_TEMP_COOL,
	PM8150B_TEMP_NORMAL,
	PM8150B_TEMP_WARM,
	PM8150B_TEMP_HOT,
};

static inline enum pm8150b_temp_zone
pm8150b_next_temp_zone(enum pm8150b_temp_zone zone, int temp_decic)
{
	if (temp_decic < 0 || (zone == PM8150B_TEMP_COLD && temp_decic < 30))
		return PM8150B_TEMP_COLD;
	if (temp_decic >= 550 || (zone == PM8150B_TEMP_HOT && temp_decic >= 520))
		return PM8150B_TEMP_HOT;
	if (temp_decic < 100 || (zone == PM8150B_TEMP_COOL && temp_decic < 120))
		return PM8150B_TEMP_COOL;
	if (temp_decic >= 450 || (zone == PM8150B_TEMP_WARM && temp_decic >= 430))
		return PM8150B_TEMP_WARM;
	return PM8150B_TEMP_NORMAL;
}

static inline bool pm8150b_use_gadget_limit(bool typec_online, int typec_ua)
{
	/* Type-C medium/high Rp supersedes USB enumeration power limits. */
	return !typec_online || typec_ua < 1500000;
}

static inline int pm8150b_parallel_fcc(int total_ua, int settled_ua,
				      int input_limit_ua, int voltage_uv,
				      int float_uv, int percent)
{
	u64 budget;
	int share;

	if (voltage_uv <= 0 || settled_ua <= 0)
		return 0;
	budget = div64_u64((u64)min(settled_ua, input_limit_ua) * voltage_uv * 80,
			  (u64)float_uv * 100);
	share = min_t(u64, total_ua, budget) * percent / 100;
	return rounddown(share, 50000);
}

#endif
