// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "pps-irq-filter.h"

#define PERIOD_NS 1000000000U
#define JITTER_NS 2000000U
#define WRAP_NS 873813333ULL

static struct pps_irq_filter pps_irq_test_filter(void)
{
	return (struct pps_irq_filter) {
		.period_ns = PERIOD_NS,
		.max_jitter_ns = JITTER_NS,
	};
}

static void pps_irq_acquire(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();

	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, PERIOD_NS));
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, 2ULL * PERIOD_NS));
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, 3ULL * PERIOD_NS));
}

static void pps_irq_comparator_wrap(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 100ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now + WRAP_NS));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter,
						       now + PERIOD_NS + WRAP_NS));
	now += 2ULL * PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_unqualified_alternation(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = PERIOD_NS;
	int i;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	for (i = 0; i < 10; i++) {
		now += WRAP_NS;
		KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
		now += 2ULL * PERIOD_NS - WRAP_NS;
		KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	}
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_recorded_edges(struct kunit *test)
{
	/* IRQ timestamps relative to the first edge in the device trace. */
	static const struct {
		u64 time_ns;
		bool accept;
	} edges[] = {
		{ 0, false },
		{ 1000167000, true },
		{ 1873982000, false },
		{ 3000001000, true },
		{ 3873814000, false },
		{ 5000002000, true },
		{ 5873814000, false },
		{ 8000191000, false },
		{ 9000166000, true },
	};
	struct pps_irq_filter filter = pps_irq_test_filter();
	int i;

	for (i = 0; i < ARRAY_SIZE(edges); i++)
		KUNIT_EXPECT_EQ_MSG(test,
				    pps_irq_filter_assert(&filter, edges[i].time_ns),
				    edges[i].accept, "edge %d", i);
}

static void pps_irq_jitter_limits(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS - JITTER_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS + JITTER_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter,
						       now + PERIOD_NS - JITTER_NS - 1));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter,
						       now + PERIOD_NS + JITTER_NS + 1));
	now += 2ULL * PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_missing_pulse(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += 2ULL * PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_outage_reacquire(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += 5ULL * PERIOD_NS + PERIOD_NS - WRAP_NS;
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_duplicate_edge(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now + 1000000));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_phase_change_reacquire(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += 2ULL * PERIOD_NS - WRAP_NS;
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_rejected_traffic_reacquire(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;
	int i;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	for (i = 0; i < 4; i++) {
		now += WRAP_NS;
		KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	}
	now = 15ULL * PERIOD_NS;
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static void pps_irq_resume_reacquire(struct kunit *test)
{
	struct pps_irq_filter filter = pps_irq_test_filter();
	u64 now = 10ULL * PERIOD_NS;

	KUNIT_ASSERT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_ASSERT_TRUE(test, pps_irq_filter_assert(&filter, now));
	pps_irq_filter_reset(&filter);
	now += PERIOD_NS - WRAP_NS;
	KUNIT_EXPECT_FALSE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
	now += PERIOD_NS;
	KUNIT_EXPECT_TRUE(test, pps_irq_filter_assert(&filter, now));
}

static struct kunit_case pps_irq_cases[] = {
	KUNIT_CASE(pps_irq_acquire),
	KUNIT_CASE(pps_irq_comparator_wrap),
	KUNIT_CASE(pps_irq_unqualified_alternation),
	KUNIT_CASE(pps_irq_recorded_edges),
	KUNIT_CASE(pps_irq_jitter_limits),
	KUNIT_CASE(pps_irq_missing_pulse),
	KUNIT_CASE(pps_irq_outage_reacquire),
	KUNIT_CASE(pps_irq_duplicate_edge),
	KUNIT_CASE(pps_irq_phase_change_reacquire),
	KUNIT_CASE(pps_irq_rejected_traffic_reacquire),
	KUNIT_CASE(pps_irq_resume_reacquire),
	{}
};

static struct kunit_suite pps_irq_suite = {
	.name = "pps-irq-phase",
	.test_cases = pps_irq_cases,
};

kunit_test_suite(pps_irq_suite);

MODULE_LICENSE("GPL");
