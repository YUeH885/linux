/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_PM8150B_POLICY_H
#define QCOM_PM8150B_POLICY_H

#include <linux/math64.h>
#include <linux/minmax.h>

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
