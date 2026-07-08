/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/debugpoint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <cmsis_core.h>
#include <cortex_m/dwt.h>

static inline volatile uint32_t *dwt_comp(int index)
{
	return &DWT->COMP0 + (uint32_t)index * 4U;
}

#if !defined(CONFIG_ARMV8_M_MAINLINE)
static inline volatile uint32_t *dwt_mask(int index)
{
	return &DWT->MASK0 + (uint32_t)index * 4U;
}
#endif

static inline volatile uint32_t *dwt_function(int index)
{
	return &DWT->FUNCTION0 + (uint32_t)index * 4U;
}

struct arm_wp_slot {
	atomic_t active;
	bool available;
	bool managed;
	struct z_debugpoint_handle handle;
	enum z_debugpoint_type type;
	uintptr_t addr;
	size_t size;
};

static struct arm_wp_slot g_slots[CONFIG_ARM_WATCHPOINT_MAX_SLOTS];
static int g_num_comparators;
static bool g_initialized;

static bool supported_type(enum z_debugpoint_type type)
{
	return type == Z_DEBUGPOINT_WATCH_READ ||
	       type == Z_DEBUGPOINT_WATCH_WRITE ||
	       type == Z_DEBUGPOINT_WATCH_RW;
}

static bool representable_range(uintptr_t addr, size_t size)
{
#if defined(CONFIG_ARMV8_M_MAINLINE)
	return (size == 1U || size == 2U || size == 4U) &&
	       (addr & (size - 1U)) == 0U;
#else
	return IS_POWER_OF_TWO(size) && (addr & (size - 1U)) == 0U;
#endif
}

static uint32_t function_mode(uint32_t function)
{
#if defined(CONFIG_ARMV8_M_MAINLINE)
	return function & DWT_FUNCTION_MATCH_Msk;
#else
	return function & DWT_FUNCTION_FUNCTION_Msk;
#endif
}

static bool comparator_enabled(int index)
{
	return function_mode(*dwt_function(index)) != 0U;
}

static int find_free_slot(void)
{
	for (int i = 0; i < g_num_comparators; i++) {
		struct arm_wp_slot *slot = &g_slots[i];

		if (!slot->available || atomic_get(&slot->active) != 0) {
			continue;
		}
		if (comparator_enabled(i)) {
			slot->available = false;
			continue;
		}

		return i;
	}

	return -ENOSPC;
}

static int arm_watchpoint_init(void)
{
	if ((DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0U) {
		return -EBUSY;
	}
#if defined(CONFIG_ARMV8_M_SE) && !defined(CONFIG_ARM_NONSECURE_FIRMWARE)
	if ((DCB->DEMCR & DCB_DEMCR_SDME_Msk) == 0U) {
		return -ENOTSUP;
	}
#endif

	z_arm_dwt_init();
	z_arm_dwt_enable_debug_monitor();
	if ((DCB->DEMCR & DCB_DEMCR_MON_EN_Msk) == 0U) {
		return -ENOTSUP;
	}

	g_num_comparators = (int)((DWT->CTRL & DWT_CTRL_NUMCOMP_Msk) >>
				  DWT_CTRL_NUMCOMP_Pos);
	g_num_comparators = MIN(g_num_comparators,
				CONFIG_ARM_WATCHPOINT_MAX_SLOTS);
	if (g_num_comparators == 0) {
		return -ENOTSUP;
	}

	for (int i = 0; i < g_num_comparators; i++) {
		g_slots[i].available = !comparator_enabled(i);
	}

	return 0;
}

static uint32_t build_function_reg(enum z_debugpoint_type type, size_t size)
{
#if defined(CONFIG_ARMV8_M_MAINLINE)
	uint32_t match = type == Z_DEBUGPOINT_WATCH_RW ? 0x4U :
			 type == Z_DEBUGPOINT_WATCH_WRITE ? 0x5U : 0x6U;
	uint32_t data_size = (uint32_t)__builtin_ctzl(size);

	return ((match << DWT_FUNCTION_MATCH_Pos) & DWT_FUNCTION_MATCH_Msk) |
	       ((1U << DWT_FUNCTION_ACTION_Pos) & DWT_FUNCTION_ACTION_Msk) |
	       ((data_size << DWT_FUNCTION_DATAVSIZE_Pos) &
		DWT_FUNCTION_DATAVSIZE_Msk);
#else
	uint32_t function = type == Z_DEBUGPOINT_WATCH_RW ? 0x7U :
			    type == Z_DEBUGPOINT_WATCH_WRITE ? 0x6U : 0x5U;

	ARG_UNUSED(size);
	return (function << DWT_FUNCTION_FUNCTION_Pos) &
	       DWT_FUNCTION_FUNCTION_Msk;
#endif
}

static uint32_t function_config_mask(void)
{
#if defined(CONFIG_ARMV8_M_MAINLINE)
	return DWT_FUNCTION_MATCH_Msk | DWT_FUNCTION_ACTION_Msk |
	       DWT_FUNCTION_DATAVSIZE_Msk;
#else
	return DWT_FUNCTION_FUNCTION_Msk;
#endif
}

static int clear_comparator(int index)
{
	*dwt_function(index) = 0U;
	__DSB();
	__ISB();
	if (function_mode(*dwt_function(index)) != 0U) {
		return -ENOTSUP;
	}

	*dwt_comp(index) = 0U;
#if !defined(CONFIG_ARMV8_M_MAINLINE)
	*dwt_mask(index) = 0U;
#endif
	__DSB();
	__ISB();
	return 0;
}

static int restore_comparator(int index)
{
	struct arm_wp_slot *slot = &g_slots[index];
	uint32_t function = build_function_reg(slot->type, slot->size);
	int ret = clear_comparator(index);

	if (ret != 0) {
		return ret;
	}

	*dwt_comp(index) = (uint32_t)slot->addr;
#if !defined(CONFIG_ARMV8_M_MAINLINE)
	uint32_t mask = slot->size == 1U ? 0U :
			(uint32_t)__builtin_ctzl(slot->size);

	*dwt_mask(index) = mask;
#endif
	*dwt_function(index) = function;
	__DSB();
	__ISB();

	bool mismatch =
		*dwt_comp(index) != (uint32_t)slot->addr ||
		(*dwt_function(index) & function_config_mask()) !=
			(function & function_config_mask());

#if !defined(CONFIG_ARMV8_M_MAINLINE)
	mismatch = mismatch || *dwt_mask(index) != mask;
#endif
	if (mismatch) {
		ret = clear_comparator(index);
		return ret != 0 ? ret : -ENOTSUP;
	}

	return 0;
}

static void release_slot(int index)
{
	atomic_set(&g_slots[index].active, 0);
	g_slots[index].managed = false;
	g_slots[index].available = true;
}

int arch_debugpoint_validate(const struct z_debugpoint *dp)
{
	if (!supported_type(dp->type)) {
		return -ENOTSUP;
	}
	if (!representable_range((uintptr_t)dp->addr, dp->size)) {
		return -ENOTSUP;
	}

	return 0;
}

int arch_debugpoint_install_local(const struct z_debugpoint *dp)
{
	unsigned int key = irq_lock();
	int ret = 0;

	if (!g_initialized) {
		ret = arm_watchpoint_init();
		if (ret != 0) {
			goto out;
		}
		g_initialized = true;
	}
	if ((DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0U) {
		ret = -EBUSY;
		goto out;
	}

	int index = find_free_slot();

	if (index < 0) {
		ret = index;
		goto out;
	}

	struct arm_wp_slot *slot = &g_slots[index];
	uint32_t function = build_function_reg(dp->type, dp->size);

	ret = clear_comparator(index);
	if (ret != 0) {
		goto out;
	}
	*dwt_comp(index) = (uint32_t)(uintptr_t)dp->addr;
#if !defined(CONFIG_ARMV8_M_MAINLINE)
	uint32_t mask = dp->size == 1U ? 0U :
			(uint32_t)__builtin_ctzl(dp->size);

	*dwt_mask(index) = mask;
	if (*dwt_mask(index) != mask) {
		ret = clear_comparator(index);
		if (ret == 0) {
			ret = -ENOTSUP;
		}
		goto out;
	}
#endif

	slot->handle = dp->handle;
	slot->type = dp->type;
	slot->addr = (uintptr_t)dp->addr;
	slot->size = dp->size;
	slot->managed = true;
	atomic_set(&slot->active, 1);

	*dwt_function(index) = function;
	__DSB();
	__ISB();

	uint32_t actual_function = *dwt_function(index);

	if (*dwt_comp(index) != (uint32_t)slot->addr ||
	    (actual_function & function_config_mask()) !=
		    (function & function_config_mask())) {
		atomic_set(&slot->active, 0);
		ret = clear_comparator(index);
		if (ret == 0) {
			release_slot(index);
			ret = -ENOTSUP;
		}
	}

out:
	irq_unlock(key);
	return ret;
}

int arch_debugpoint_uninstall_local(const struct z_debugpoint *dp)
{
	unsigned int key = irq_lock();
	int ret = -ENOENT;

	for (int i = 0; i < g_num_comparators; i++) {
		struct arm_wp_slot *slot = &g_slots[i];

		if (atomic_get(&slot->active) == 0 ||
		    slot->handle.slot != dp->handle.slot ||
		    slot->handle.generation != dp->handle.generation) {
			continue;
		}

		ret = clear_comparator(i);
		if (ret == 0) {
			release_slot(i);
		}

		break;
	}

	irq_unlock(key);
	return ret;
}

static int restore_active_comparators(void)
{
	int ret = 0;

	for (int i = 0; i < g_num_comparators; i++) {
		struct arm_wp_slot *slot = &g_slots[i];

		if (!slot->managed || atomic_get(&slot->active) == 0) {
			continue;
		}

		int slot_ret = restore_comparator(i);

		if (ret == 0 && slot_ret != 0) {
			ret = slot_ret;
		}
	}

	return ret;
}

int arch_debugpoint_cpu_sync(void)
{
	unsigned int key = irq_lock();
	int ret = 0;

	if (!g_initialized) {
		goto out;
	}
	if ((DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0U) {
		ret = -EBUSY;
		goto out;
	}
#if defined(CONFIG_ARMV8_M_SE) && !defined(CONFIG_ARM_NONSECURE_FIRMWARE)
	if ((DCB->DEMCR & DCB_DEMCR_SDME_Msk) == 0U) {
		ret = -ENOTSUP;
		goto out;
	}
#endif

	z_arm_dwt_init();
	z_arm_dwt_enable_debug_monitor();
	if ((DCB->DEMCR & DCB_DEMCR_MON_EN_Msk) == 0U) {
		ret = -ENOTSUP;
		goto out;
	}
	ret = restore_active_comparators();

out:
	irq_unlock(key);
	return ret;
}

int z_arm_debugpoint_handle(struct arch_esf *esf)
{
	if (!g_initialized || g_num_comparators == 0 ||
	    (SCB->DFSR & SCB_DFSR_DWTTRAP_Msk) == 0U) {
		return -ENOENT;
	}

	uint32_t functions[CONFIG_ARM_WATCHPOINT_MAX_SLOTS] = {0U};

	for (int i = 0; i < g_num_comparators; i++) {
		if (!g_slots[i].managed) {
			continue;
		}

		functions[i] = *dwt_function(i);
		*dwt_function(i) = 0U;
	}
	__DSB();
	__ISB();

	for (int i = 0; i < g_num_comparators; i++) {
		if (g_slots[i].managed &&
		    function_mode(*dwt_function(i)) != 0U) {
			int ret = restore_active_comparators();

			return ret != 0 ? ret : -ENOTSUP;
		}
	}

	bool handled = false;

	for (int i = 0; i < g_num_comparators; i++) {
		struct arm_wp_slot *slot = &g_slots[i];

		if (!slot->managed ||
		    (functions[i] & DWT_FUNCTION_MATCHED_Msk) == 0U) {
			continue;
		}

		handled = true;
		if (atomic_get(&slot->active) == 0) {
			int ret = clear_comparator(i);

			if (ret != 0) {
				(void)restore_active_comparators();
				return ret;
			}
			release_slot(i);
			continue;
		}

		struct z_debugpoint_event event = {
			.pc = (void *)(uintptr_t)esf->basic.pc,
			.access_addr = NULL,
			.access_addr_valid = false,
			.access_size = 0U,
			.type = slot->type,
			.timing = Z_DEBUGPOINT_TIMING_AFTER,
			.esf = esf,
		};

		z_debugpoint_hit(slot->handle, &event, false);
	}

	int ret = restore_active_comparators();

	if (ret != 0) {
		return ret;
	}
	if (!handled) {
		return -ENOENT;
	}

	SCB->DFSR = SCB_DFSR_DWTTRAP_Msk;
	__DSB();
	__ISB();
	return 0;
}
