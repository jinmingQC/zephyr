/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* AArch64 hardware watchpoint backend. */
#include <zephyr/kernel.h>
#include <zephyr/arch/arm64/lib_helpers.h>
#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/arch/debugpoint.h>
#include <kernel_arch_interface.h>

#define DBGWCR_E           BIT(0)
#define DBGWCR_PAC_EL1     BIT(1)
#define DBGWCR_PAC_EL0     BIT(2)
#define DBGWCR_PAC_ALL     (DBGWCR_PAC_EL1 | DBGWCR_PAC_EL0)
#define DBGWCR_LSC_SHIFT   3
#define DBGWCR_LSC_LOAD    (1ULL << DBGWCR_LSC_SHIFT)
#define DBGWCR_LSC_STORE   (2ULL << DBGWCR_LSC_SHIFT)
#define DBGWCR_LSC_BOTH    (3ULL << DBGWCR_LSC_SHIFT)
#define DBGWCR_LSC_MASK    (3ULL << DBGWCR_LSC_SHIFT)
#define DBGWCR_BAS_SHIFT   5
#define DBGWCR_BAS_MASK    (0xffULL << DBGWCR_BAS_SHIFT)
#define DBGWCR_CONFIG_MASK (DBGWCR_E | DBGWCR_PAC_ALL | DBGWCR_LSC_MASK | DBGWCR_BAS_MASK)

/* WRPs is encoded as the comparator count minus one. */
#define DFR0_WRPS_SHIFT 20
#define DFR0_WRPS_MASK  0xfULL

#define MDSCR_SS           BIT(0)
#define MDSCR_KDE          BIT(13)
#define MDSCR_MDE          BIT(15)
#define SPSR_SS            BIT(21)

#define ESR_EC_STEP_LOWER     0x32U
#define ESR_EC_STEP_SAME      0x33U
#define ESR_EC_WATCHPT_LOWER  0x34U
#define ESR_EC_WATCHPT_SAME   0x35U
#define ESR_ISS_FNV             BIT(10)

/*
 * Indexed access to DBGWVR<n>_EL1 / DBGWCR<n>_EL1 via MRS/MSR.
 * There are no C-accessible register arrays; we use a switch over all
 * 16 possible indices, expanding MRS/MSR at compile time.
 */
#define _ARM64_WR_SET_CASE(reg, n, val) \
	case n: \
		write_sysreg(val, dbg##reg##n##_el1); \
		break;
#define _ARM64_WR_GET_CASE(reg, n, val) \
	case n: \
		(val) = read_sysreg(dbg##reg##n##_el1); \
		break;

#define ARM64_DBGWVR_SET(n, val) do { switch (n) {      \
	_ARM64_WR_SET_CASE(wvr, 0,  val)                \
	_ARM64_WR_SET_CASE(wvr, 1,  val)                \
	_ARM64_WR_SET_CASE(wvr, 2,  val)                \
	_ARM64_WR_SET_CASE(wvr, 3,  val)                \
	_ARM64_WR_SET_CASE(wvr, 4,  val)                \
	_ARM64_WR_SET_CASE(wvr, 5,  val)                \
	_ARM64_WR_SET_CASE(wvr, 6,  val)                \
	_ARM64_WR_SET_CASE(wvr, 7,  val)                \
	_ARM64_WR_SET_CASE(wvr, 8,  val)                \
	_ARM64_WR_SET_CASE(wvr, 9,  val)                \
	_ARM64_WR_SET_CASE(wvr, 10, val)                \
	_ARM64_WR_SET_CASE(wvr, 11, val)                \
	_ARM64_WR_SET_CASE(wvr, 12, val)                \
	_ARM64_WR_SET_CASE(wvr, 13, val)                \
	_ARM64_WR_SET_CASE(wvr, 14, val)                \
	_ARM64_WR_SET_CASE(wvr, 15, val)                \
} } while (0)

#define ARM64_DBGWCR_SET(n, val) do { switch (n) {      \
	_ARM64_WR_SET_CASE(wcr, 0,  val)                \
	_ARM64_WR_SET_CASE(wcr, 1,  val)                \
	_ARM64_WR_SET_CASE(wcr, 2,  val)                \
	_ARM64_WR_SET_CASE(wcr, 3,  val)                \
	_ARM64_WR_SET_CASE(wcr, 4,  val)                \
	_ARM64_WR_SET_CASE(wcr, 5,  val)                \
	_ARM64_WR_SET_CASE(wcr, 6,  val)                \
	_ARM64_WR_SET_CASE(wcr, 7,  val)                \
	_ARM64_WR_SET_CASE(wcr, 8,  val)                \
	_ARM64_WR_SET_CASE(wcr, 9,  val)                \
	_ARM64_WR_SET_CASE(wcr, 10, val)                \
	_ARM64_WR_SET_CASE(wcr, 11, val)                \
	_ARM64_WR_SET_CASE(wcr, 12, val)                \
	_ARM64_WR_SET_CASE(wcr, 13, val)                \
	_ARM64_WR_SET_CASE(wcr, 14, val)                \
	_ARM64_WR_SET_CASE(wcr, 15, val)                \
} } while (0)

#define ARM64_DBGWCR_GET(n, val) do { switch (n) {      \
	_ARM64_WR_GET_CASE(wcr, 0,  val)                \
	_ARM64_WR_GET_CASE(wcr, 1,  val)                \
	_ARM64_WR_GET_CASE(wcr, 2,  val)                \
	_ARM64_WR_GET_CASE(wcr, 3,  val)                \
	_ARM64_WR_GET_CASE(wcr, 4,  val)                \
	_ARM64_WR_GET_CASE(wcr, 5,  val)                \
	_ARM64_WR_GET_CASE(wcr, 6,  val)                \
	_ARM64_WR_GET_CASE(wcr, 7,  val)                \
	_ARM64_WR_GET_CASE(wcr, 8,  val)                \
	_ARM64_WR_GET_CASE(wcr, 9,  val)                \
	_ARM64_WR_GET_CASE(wcr, 10, val)                \
	_ARM64_WR_GET_CASE(wcr, 11, val)                \
	_ARM64_WR_GET_CASE(wcr, 12, val)                \
	_ARM64_WR_GET_CASE(wcr, 13, val)                \
	_ARM64_WR_GET_CASE(wcr, 14, val)                \
	_ARM64_WR_GET_CASE(wcr, 15, val)                \
} } while (0)

#define ARM64_DBGWVR_GET(n, val) do { switch (n) {  \
	_ARM64_WR_GET_CASE(wvr, 0,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 1,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 2,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 3,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 4,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 5,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 6,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 7,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 8,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 9,  val)                   \
	_ARM64_WR_GET_CASE(wvr, 10, val)                   \
	_ARM64_WR_GET_CASE(wvr, 11, val)                   \
	_ARM64_WR_GET_CASE(wvr, 12, val)                   \
	_ARM64_WR_GET_CASE(wvr, 13, val)                   \
	_ARM64_WR_GET_CASE(wvr, 14, val)                   \
	_ARM64_WR_GET_CASE(wvr, 15, val)                   \
} } while (0)

struct arm64_wp_slot {
	atomic_t active;
	bool available;
	struct z_debugpoint_handle handle;
	enum z_debugpoint_type type;
	uintptr_t addr;
	size_t size;
	uint64_t wvr;
	uint64_t wcr;
};

static struct arm64_wp_slot g_slots[CONFIG_ARM64_WATCHPOINT_MAX_SLOTS];
static int g_num_wrps;
static atomic_t g_initialized;
static atomic_t g_cpu_owned[CONFIG_MP_MAX_NUM_CPUS];
static atomic_t g_cpu_stepping[CONFIG_MP_MAX_NUM_CPUS];
static uint8_t g_cpu_saved_step[CONFIG_MP_MAX_NUM_CPUS];
static struct k_spinlock g_lock;

BUILD_ASSERT(CONFIG_ARM64_WATCHPOINT_MAX_SLOTS <= ATOMIC_BITS);

static bool supported_type(enum z_debugpoint_type type)
{
	return type == Z_DEBUGPOINT_WATCH_READ ||
	       type == Z_DEBUGPOINT_WATCH_WRITE ||
	       type == Z_DEBUGPOINT_WATCH_RW;
}

static bool range_contains(uintptr_t addr, size_t size, uintptr_t value)
{
	return value >= addr && value - addr < size;
}

static uint64_t build_wcr(enum z_debugpoint_type type, uint8_t bas)
{
	uint64_t lsc = type == Z_DEBUGPOINT_WATCH_RW ? DBGWCR_LSC_BOTH :
		       type == Z_DEBUGPOINT_WATCH_WRITE ? DBGWCR_LSC_STORE :
		       DBGWCR_LSC_LOAD;

	return DBGWCR_E | DBGWCR_PAC_ALL | lsc |
	       ((uint64_t)bas << DBGWCR_BAS_SHIFT);
}

static int cpu_num_wrps(void)
{
	return (int)(((read_sysreg(id_aa64dfr0_el1) >> DFR0_WRPS_SHIFT) &
		      DFR0_WRPS_MASK) + 1U);
}

static bool cpu_owns_slot(unsigned int cpu, int slot)
{
	return atomic_test_bit(&g_cpu_owned[cpu], slot);
}

static int clear_cpu_slot(unsigned int cpu, int slot)
{
	uint64_t wcr = 0U;

	ARM64_DBGWCR_SET(slot, 0);
	barrier_isync_fence_full();
	ARM64_DBGWCR_GET(slot, wcr);
	if ((wcr & DBGWCR_E) != 0U) {
		return -ENOTSUP;
	}

	ARM64_DBGWVR_SET(slot, 0);
	barrier_isync_fence_full();
	atomic_clear_bit(&g_cpu_owned[cpu], slot);
	return 0;
}

static bool wcr_matches(uint64_t actual, uint64_t expected)
{
	return (actual & DBGWCR_CONFIG_MASK) ==
	       (expected & DBGWCR_CONFIG_MASK);
}

static int program_cpu_slot(unsigned int cpu, int slot, uint64_t wvr,
			    uint64_t wcr)
{
	if (!cpu_owns_slot(cpu, slot)) {
		uint64_t current_wcr = 0U;

		ARM64_DBGWCR_GET(slot, current_wcr);
		if ((current_wcr & DBGWCR_E) != 0U) {
			return -EBUSY;
		}
	}
	atomic_set_bit(&g_cpu_owned[cpu], slot);
	ARM64_DBGWCR_SET(slot, 0);
	ARM64_DBGWVR_SET(slot, wvr);
	ARM64_DBGWCR_SET(slot, wcr);
	barrier_isync_fence_full();

	uint64_t actual_wvr = 0U;
	uint64_t actual_wcr = 0U;

	ARM64_DBGWVR_GET(slot, actual_wvr);
	ARM64_DBGWCR_GET(slot, actual_wcr);
	if (actual_wvr != wvr || !wcr_matches(actual_wcr, wcr)) {
		int ret = clear_cpu_slot(cpu, slot);

		return ret != 0 ? ret : -ENOTSUP;
	}

	return 0;
}

static int suspend_cpu_watchpoints(unsigned int cpu)
{
	for (int i = 0; i < g_num_wrps; i++) {
		if (!cpu_owns_slot(cpu, i)) {
			continue;
		}

		uint64_t wcr = 0U;

		ARM64_DBGWCR_GET(i, wcr);
		ARM64_DBGWCR_SET(i, wcr & ~DBGWCR_E);
	}
	barrier_isync_fence_full();

	for (int i = 0; i < g_num_wrps; i++) {
		uint64_t wcr = 0U;

		if (!cpu_owns_slot(cpu, i)) {
			continue;
		}
		ARM64_DBGWCR_GET(i, wcr);
		if ((wcr & DBGWCR_E) != 0U) {
			return -ENOTSUP;
		}
	}

	return 0;
}

static int restore_cpu_watchpoints(unsigned int cpu)
{
	for (int i = 0; i < g_num_wrps; i++) {
		struct arm64_wp_slot *slot = &g_slots[i];
		int ret;

		if (!cpu_owns_slot(cpu, i)) {
			continue;
		}
		if (atomic_get(&slot->active) != 0) {
			ret = program_cpu_slot(cpu, i, slot->wvr, slot->wcr);
			if (ret == 0 && atomic_get(&slot->active) == 0) {
				ret = clear_cpu_slot(cpu, i);
			}
		} else {
			ret = clear_cpu_slot(cpu, i);
		}
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int enable_monitor_debug(void)
{
	uint64_t mdscr = read_sysreg(mdscr_el1);
	uint64_t enable = MDSCR_KDE | MDSCR_MDE;

	write_sysreg(mdscr | enable, mdscr_el1);
	barrier_isync_fence_full();
	return (read_sysreg(mdscr_el1) & enable) == enable ? 0 : -ENOTSUP;
}

static bool foreign_watchpoint_enabled(unsigned int cpu)
{
	int num_wrps = cpu_num_wrps();

	for (int i = 0; i < num_wrps; i++) {
		if (i < g_num_wrps && cpu_owns_slot(cpu, i)) {
			continue;
		}

		uint64_t wcr = 0U;

		ARM64_DBGWCR_GET(i, wcr);
		if ((wcr & DBGWCR_E) != 0U) {
			return true;
		}
	}

	return false;
}

static int arm64_watchpoint_init(void)
{
	unsigned int cpu = arch_curr_cpu()->id;

	g_num_wrps = MIN(cpu_num_wrps(),
			 CONFIG_ARM64_WATCHPOINT_MAX_SLOTS);
	if (g_num_wrps <= 0) {
		return -ENOTSUP;
	}

	for (int i = 0; i < g_num_wrps; i++) {
		uint64_t wcr = 0U;

		ARM64_DBGWCR_GET(i, wcr);
		g_slots[i].available = (wcr & DBGWCR_E) == 0U;
	}

	int ret = enable_monitor_debug();

	if (ret != 0) {
		return ret;
	}
	atomic_set(&g_cpu_owned[cpu], 0);
	atomic_set(&g_cpu_stepping[cpu], 0);
	return 0;
}

int arch_debugpoint_validate(const struct z_debugpoint *dp)
{
	return supported_type(dp->type) ? 0 : -ENOTSUP;
}

int arch_debugpoint_install_local(const struct z_debugpoint *dp)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);
	unsigned int cpu = arch_curr_cpu()->id;
	int ret = 0;

	if (atomic_get(&g_initialized) == 0) {
		ret = arm64_watchpoint_init();
		if (ret != 0) {
			goto out;
		}
		atomic_set(&g_initialized, 1);
	}

	uintptr_t addr = (uintptr_t)dp->addr;
	uintptr_t last = addr + dp->size - 1U;
	uintptr_t base = addr & ~(uintptr_t)0x7U;
	uintptr_t last_base = last & ~(uintptr_t)0x7U;
	size_t chunks = (last_base - base) / 8U + 1U;
	size_t free_count = 0U;

	for (int i = 0; i < g_num_wrps; i++) {
		if (g_slots[i].available &&
		    atomic_get(&g_slots[i].active) == 0 &&
		    !cpu_owns_slot(cpu, i)) {
			free_count++;
		}
	}
	if (chunks > free_count) {
		ret = -ENOSPC;
		goto out;
	}

	size_t configured = 0U;

	for (int i = 0; i < g_num_wrps && configured < chunks; i++) {
		struct arm64_wp_slot *slot = &g_slots[i];

		if (!slot->available || atomic_get(&slot->active) != 0 ||
		    cpu_owns_slot(cpu, i)) {
			continue;
		}

		uintptr_t chunk_base = base + configured * 8U;
		uint8_t bas = 0U;

		for (int byte = 0; byte < 8; byte++) {
			uintptr_t byte_addr = chunk_base + (uintptr_t)byte;

			if (range_contains(addr, dp->size, byte_addr)) {
				bas |= BIT(byte);
			}
		}

		slot->handle = dp->handle;
		slot->type = dp->type;
		slot->addr = addr;
		slot->size = dp->size;
		slot->wvr = chunk_base;
		slot->wcr = build_wcr(dp->type, bas);
		atomic_set(&slot->active, 1);

		ret = program_cpu_slot(cpu, i, slot->wvr, slot->wcr);
		if (ret != 0) {
			atomic_set(&slot->active, 0);
			break;
		}
		configured++;
	}

	if (ret != 0) {
		int cleanup_ret = 0;

		for (int i = 0; i < g_num_wrps; i++) {
			struct arm64_wp_slot *slot = &g_slots[i];

			if (slot->handle.slot != dp->handle.slot ||
			    slot->handle.generation != dp->handle.generation) {
				continue;
			}

			atomic_set(&slot->active, 0);
			if (cpu_owns_slot(cpu, i)) {
				int clear_ret = clear_cpu_slot(cpu, i);

				if (cleanup_ret == 0 && clear_ret != 0) {
					cleanup_ret = clear_ret;
				}
			}

		}
		if (cleanup_ret != 0) {
			ret = cleanup_ret;
		}
	}

out:
	k_spin_unlock(&g_lock, key);
	return ret;
}

int arch_debugpoint_uninstall_local(const struct z_debugpoint *dp)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);
	unsigned int cpu = arch_curr_cpu()->id;
	uint32_t matching = 0U;
	uint32_t cleared = 0U;
	int ret = -ENOENT;

	for (int i = 0; i < g_num_wrps; i++) {
		struct arm64_wp_slot *slot = &g_slots[i];

		if (atomic_get(&slot->active) == 0 ||
		    slot->handle.slot != dp->handle.slot ||
		    slot->handle.generation != dp->handle.generation) {
			continue;
		}

		matching |= BIT(i);
		ret = 0;
		if (cpu_owns_slot(cpu, i)) {
			ret = clear_cpu_slot(cpu, i);
			if (ret != 0) {
				break;
			}
			cleared |= BIT(i);
		}
	}

	if (ret != 0 && ret != -ENOENT) {
		for (int i = 0; i < g_num_wrps; i++) {
			if ((cleared & BIT(i)) == 0U) {
				continue;
			}

			int restore_ret = program_cpu_slot(cpu, i, g_slots[i].wvr,
							   g_slots[i].wcr);

			if (restore_ret != 0) {
				ret = restore_ret;
			}
		}
	} else if (matching != 0U) {
		for (int i = 0; i < g_num_wrps; i++) {
			if ((matching & BIT(i)) != 0U) {
				atomic_set(&g_slots[i].active, 0);
			}
		}
		ret = 0;
	}

	k_spin_unlock(&g_lock, key);
	return ret;
}

int arch_debugpoint_cpu_sync(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	if (atomic_get(&g_initialized) == 0) {
		k_spin_unlock(&g_lock, key);
		return 0;
	}

	unsigned int cpu = arch_curr_cpu()->id;
	int num_wrps = MIN(cpu_num_wrps(),
			   CONFIG_ARM64_WATCHPOINT_MAX_SLOTS);
	int ret = enable_monitor_debug();

	if (ret != 0) {
		k_spin_unlock(&g_lock, key);
		return ret;
	}

	for (int i = 0; i < g_num_wrps; i++) {
		struct arm64_wp_slot *slot = &g_slots[i];

		if (i >= num_wrps) {
			if (atomic_get(&slot->active) != 0) {
				ret = -ENOTSUP;
			}
			continue;
		}

		if (atomic_get(&slot->active) != 0) {
			ret = program_cpu_slot(cpu, i, slot->wvr,
					       slot->wcr);
			if (ret != 0) {
				break;
			}
		} else if (cpu_owns_slot(cpu, i)) {
			ret = clear_cpu_slot(cpu, i);
			if (ret != 0) {
				break;
			}
		}
	}
	if (ret == 0 && atomic_get(&g_cpu_stepping[cpu]) != 0) {
		ret = suspend_cpu_watchpoints(cpu);
	} else {
		barrier_isync_fence_full();
	}

	k_spin_unlock(&g_lock, key);
	return ret;
}

static bool access_matches(enum z_debugpoint_type configured,
			   enum z_debugpoint_type access)
{
	return configured == Z_DEBUGPOINT_WATCH_RW || configured == access;
}

static uintptr_t range_distance(uintptr_t addr, size_t size, uintptr_t value)
{
	if (range_contains(addr, size, value)) {
		return 0U;
	}

	uintptr_t last = addr + size - 1U;

	return value < addr ? addr - value : value - last;
}

static bool same_handle(struct z_debugpoint_handle a,
			struct z_debugpoint_handle b)
{
	return a.slot == b.slot && a.generation == b.generation;
}

static bool append_unique_handle(struct z_debugpoint_handle *handles,
				 size_t *count,
				 struct z_debugpoint_handle handle)
{
	for (size_t i = 0U; i < *count; i++) {
		if (same_handle(handles[i], handle)) {
			return false;
		}
	}

	handles[(*count)++] = handle;
	return true;
}

static int begin_watchpoint_step(struct arch_esf *esf)
{
	unsigned int cpu = arch_curr_cpu()->id;
	uint64_t mdscr = read_sysreg(mdscr_el1);
	uint8_t saved = 0U;
	int ret = suspend_cpu_watchpoints(cpu);

	if (ret != 0) {
		(void)restore_cpu_watchpoints(cpu);
		return ret;
	}
	if ((mdscr & MDSCR_SS) != 0U) {
		saved |= BIT(0);
	}
	if ((esf->spsr & SPSR_SS) != 0U) {
		saved |= BIT(1);
	}
	g_cpu_saved_step[cpu] = saved;
	atomic_set(&g_cpu_stepping[cpu], 1);
	esf->spsr |= SPSR_SS;
	write_sysreg(mdscr | MDSCR_SS | MDSCR_KDE | MDSCR_MDE, mdscr_el1);
	barrier_isync_fence_full();
	return 0;
}

static int complete_watchpoint_step(struct arch_esf *esf)
{
	unsigned int cpu = arch_curr_cpu()->id;

	if (!atomic_cas(&g_cpu_stepping[cpu], 1, 0)) {
		return -ENOENT;
	}

	uint64_t mdscr = read_sysreg(mdscr_el1);
	uint8_t saved = g_cpu_saved_step[cpu];

	g_cpu_saved_step[cpu] = 0U;
	mdscr = (mdscr & ~MDSCR_SS) |
		((saved & BIT(0)) != 0U ? MDSCR_SS : 0U);
	esf->spsr = (esf->spsr & ~SPSR_SS) |
		    ((saved & BIT(1)) != 0U ? SPSR_SS : 0U);
	write_sysreg(mdscr, mdscr_el1);
	barrier_isync_fence_full();
	return restore_cpu_watchpoints(cpu);
}

int z_arm64_debugpoint_handle(struct arch_esf *esf, uint64_t esr, uint64_t far)
{
	if (atomic_get(&g_initialized) == 0 || g_num_wrps == 0) {
		return -ENOENT;
	}

	uint64_t ec = GET_ESR_EC(esr);

	if (ec == ESR_EC_STEP_LOWER || ec == ESR_EC_STEP_SAME) {
		return complete_watchpoint_step(esf);
	}
	if (ec != ESR_EC_WATCHPT_LOWER && ec != ESR_EC_WATCHPT_SAME) {
		return -ENOENT;
	}

	uint64_t iss = GET_ESR_ISS(esr);
	enum z_debugpoint_type access =
		(iss & BIT(6)) != 0U ?
		Z_DEBUGPOINT_WATCH_WRITE : Z_DEBUGPOINT_WATCH_READ;
	bool access_addr_valid = (iss & ESR_ISS_FNV) == 0U;
	struct z_debugpoint_handle exact[CONFIG_ARM64_WATCHPOINT_MAX_SLOTS];
	struct z_debugpoint_handle closest = { .slot = -1 };
	uintptr_t closest_distance = UINTPTR_MAX;
	size_t exact_count = 0U;
	unsigned int cpu = arch_curr_cpu()->id;
	bool foreign_enabled = foreign_watchpoint_enabled(cpu);
	bool cleared_stale = false;

	for (int i = 0; i < g_num_wrps; i++) {
		struct arm64_wp_slot *slot = &g_slots[i];

		if (!cpu_owns_slot(cpu, i)) {
			continue;
		}

		if (atomic_get(&slot->active) == 0) {
			uint64_t wcr = 0U;

			ARM64_DBGWCR_GET(i, wcr);
			if ((wcr & DBGWCR_E) != 0U) {
				int ret = clear_cpu_slot(cpu, i);

				if (ret != 0) {
					return ret;
				}
				cleared_stale = true;
			}
			continue;
		}

		if (!access_matches(slot->type, access)) {
			continue;
		}
		if (!access_addr_valid) {
			if (!foreign_enabled) {
				(void)append_unique_handle(exact, &exact_count,
							   slot->handle);
			}
			continue;
		}

		uintptr_t distance =
			range_distance(slot->addr, slot->size, (uintptr_t)far);

		if (distance == 0U) {
			(void)append_unique_handle(exact, &exact_count,
						   slot->handle);
		} else if (exact_count == 0U && distance < closest_distance) {
			closest = slot->handle;
			closest_distance = distance;
		}
	}

	if (cleared_stale) {
		barrier_isync_fence_full();
	}

	struct z_debugpoint_event event = {
		.pc = (void *)esf->elr,
		.access_addr = access_addr_valid ?
			       (void *)(uintptr_t)far : NULL,
		.access_addr_valid = access_addr_valid,
		.access_size = 0U,
		.type = access,
		.timing = Z_DEBUGPOINT_TIMING_BEFORE,
		.esf = esf,
	};

	if (exact_count != 0U) {
		int ret = begin_watchpoint_step(esf);

		if (ret != 0) {
			return ret;
		}
		for (size_t i = 0U; i < exact_count; i++) {
			z_debugpoint_hit(exact[i], &event, false);
		}
		return 0;
	}

	if (closest.slot >= 0 && !foreign_enabled) {
		int ret = begin_watchpoint_step(esf);

		if (ret != 0) {
			return ret;
		}
		z_debugpoint_hit(closest, &event, false);
		return 0;
	}

	return cleared_stale && !foreign_enabled ? 0 : -ENOENT;
}
