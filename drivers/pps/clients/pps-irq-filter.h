/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PPS_IRQ_FILTER_H
#define PPS_IRQ_FILTER_H

#include <linux/math64.h>
#include <linux/types.h>

struct pps_irq_filter {
	u32 period_ns;
	u32 max_jitter_ns;
	u64 last_edge;
	u64 last_assert;
	enum {
		PPS_IRQ_WAITING,
		PPS_IRQ_ACQUIRING,
		PPS_IRQ_LOCKED,
	} state;
};

static inline void pps_irq_filter_reset(struct pps_irq_filter *filter)
{
	filter->state = PPS_IRQ_WAITING;
}

static inline bool pps_irq_filter_assert(struct pps_irq_filter *filter, u64 now)
{
	u64 interval = now - filter->last_edge;
	u32 phase;

	filter->last_edge = now;
	if (filter->state == PPS_IRQ_WAITING ||
	    interval > 2ULL * filter->period_ns + filter->max_jitter_ns) {
		filter->state = PPS_IRQ_ACQUIRING;
		return false;
	}

	if (filter->state == PPS_IRQ_ACQUIRING) {
		if (interval < filter->period_ns - filter->max_jitter_ns ||
		    interval > (u64)filter->period_ns + filter->max_jitter_ns)
			return false;
		filter->state = PPS_IRQ_LOCKED;
	} else {
		interval = now - filter->last_assert;
		if (interval < filter->period_ns - filter->max_jitter_ns)
			return false;

		/* Missing pulses preserve the phase of the last valid assert. */
		div_u64_rem(interval, filter->period_ns, &phase);
		if (phase > filter->max_jitter_ns &&
		    filter->period_ns - phase > filter->max_jitter_ns)
			return false;
	}

	filter->last_assert = now;
	return true;
}

#endif
