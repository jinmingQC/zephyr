/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define WORKER_COUNT 2
#define ITERATIONS   4096U
#define STACK_SIZE   (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, WORKER_COUNT, STACK_SIZE);
static struct k_thread workers[WORKER_COUNT];
static struct k_sem counting_sem;

static void give_and_take(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t i = 0U; i < ITERATIONS; i++) {
		k_sem_give(&counting_sem);
		zassert_ok(k_sem_take(&counting_sem, K_NO_WAIT));
	}
}

static void give_only(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t i = 0U; i < ITERATIONS; i++) {
		k_sem_give(&counting_sem);
	}
}

static void run_workers(k_thread_entry_t entry)
{
	for (size_t i = 0U; i < ARRAY_SIZE(workers); i++) {
		k_thread_create(&workers[i], worker_stacks[i],
				K_THREAD_STACK_SIZEOF(worker_stacks[i]), entry, NULL, NULL, NULL,
				K_PRIO_PREEMPT(0), 0, K_FOREVER);
#if defined(CONFIG_SCHED_CPU_MASK) && defined(CONFIG_SMP)
		zassert_ok(k_thread_cpu_pin(&workers[i], i % CONFIG_MP_MAX_NUM_CPUS));
#endif
	}

	for (size_t i = 0U; i < ARRAY_SIZE(workers); i++) {
		k_thread_start(&workers[i]);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(workers); i++) {
		zassert_ok(k_thread_join(&workers[i], K_SECONDS(10)));
	}
}

ZTEST(semaphore_positive_count, test_concurrent_give_take)
{
	/*
	 * Each worker gives before taking, so the count stays positive. The
	 * limit leaves room for every worker's outstanding give without loss.
	 */
	zassert_ok(k_sem_init(&counting_sem, 1U, WORKER_COUNT + 1U));
	run_workers(give_and_take);
	zassert_equal(k_sem_count_get(&counting_sem), 1U);
	zassert_ok(k_sem_take(&counting_sem, K_NO_WAIT));
	zassert_equal(k_sem_take(&counting_sem, K_NO_WAIT), -EBUSY);
}

ZTEST(semaphore_positive_count, test_concurrent_give)
{
	zassert_ok(k_sem_init(&counting_sem, 1U, 1U + WORKER_COUNT * ITERATIONS));
	run_workers(give_only);
	zassert_equal(k_sem_count_get(&counting_sem), 1U + WORKER_COUNT * ITERATIONS);
}

ZTEST(semaphore_positive_count, test_concurrent_give_at_limit)
{
	zassert_ok(k_sem_init(&counting_sem, 1U, 1U));
	run_workers(give_only);
	zassert_equal(k_sem_count_get(&counting_sem), 1U);
	zassert_ok(k_sem_take(&counting_sem, K_NO_WAIT));
	zassert_equal(k_sem_take(&counting_sem, K_NO_WAIT), -EBUSY);
}

ZTEST_SUITE(semaphore_positive_count, NULL, NULL, NULL, NULL, NULL);
