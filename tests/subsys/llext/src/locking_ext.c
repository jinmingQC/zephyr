/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/ztest_assert.h>

#if !defined(CONFIG_CRITICAL_SECTION_MONITOR) &&                                                   \
	(!defined(CONFIG_SPIN_LOCK_TIME_LIMIT) || (CONFIG_SPIN_LOCK_TIME_LIMIT == 0))
/* Catch even unreachable counter references in the unoptimized extension. */
extern uint32_t sys_clock_cycle_get_32(void)
	__attribute__((error("disabled spinlock timing must not reference the cycle counter")));
#endif

static struct k_spinlock lock;

void test_entry(void)
{
	unsigned int irq_key = k_irq_lock();

	k_irq_unlock(irq_key);

	k_spinlock_key_t key = k_spin_lock(&lock);

	k_spin_unlock(&lock, key);
	zassert_ok(k_spin_trylock(&lock, &key));
	k_spin_unlock(&lock, key);

	irq_key = k_irq_lock();
	(void)k_spin_lock(&lock);
	k_spin_release(&lock);
	k_irq_unlock(irq_key);
}
EXPORT_SYMBOL(test_entry);
