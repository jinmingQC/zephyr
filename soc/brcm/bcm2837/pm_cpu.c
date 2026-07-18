/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/sys/barrier.h>

#define SPIN_TABLE_BASE	0xd8UL
#define SPIN_TABLE_STRIDE	8UL

int pm_cpu_on(unsigned long cpuid, uintptr_t entry_point)
{
	unsigned int core = cpuid & 0xffU;
	volatile uint64_t *release;

	if (core == 0U || core >= CONFIG_MP_MAX_NUM_CPUS) {
		return -EINVAL;
	}

	release = (volatile uint64_t *)(SPIN_TABLE_BASE +
					(core * SPIN_TABLE_STRIDE));

	/*
	 * The Raspberry Pi firmware parks each secondary core in a loop which
	 * waits for its 64-bit release slot to become non-zero.
	 */
	*release = entry_point;
	barrier_dsync_fence_full();
	sev();

	return 0;
}
