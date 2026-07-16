/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/debug/watchpoint.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/arch_interface.h>
#if defined(CONFIG_ARM64)
#include <zephyr/arch/arm64/lib_helpers.h>
#endif
#if defined(CONFIG_RISCV)
#include <zephyr/arch/riscv/csr.h>
#endif
#include <zephyr/sys/atomic.h>
#include <stdint.h>
#include <limits.h>

static volatile uint32_t g_watched __aligned(4);
static volatile uint32_t g_unwatched;
static volatile uint32_t g_sink;
static volatile uint8_t g_bytes[16] __aligned(8);
static volatile uint8_t g_multi[2] __aligned(2);
static volatile uint32_t g_timing_value __aligned(4);
static volatile uint32_t g_timing_seen;
#if defined(CONFIG_USERSPACE)
#define USER_STACK_SIZE 2048
K_APPMEM_PARTITION_DEFINE(watchpoint_user_partition);
K_APP_DMEM(watchpoint_user_partition)
static volatile uint32_t g_user_watched __aligned(4);
static K_THREAD_STACK_DEFINE(g_user_stack, USER_STACK_SIZE);
static struct k_thread g_user_thread;
static struct k_mem_domain g_user_domain;
#endif

static atomic_t g_cb_count;
static void *g_cb_pc;
static void *g_cb_addr;
static bool g_cb_addr_valid;
static uint32_t g_cb_flags;
static enum k_watchpoint_timing g_cb_timing;
static bool g_cb_rearm_required;
static unsigned int g_cb_cpu;
#if defined(CONFIG_WATCHPOINT_CALLSTACK)
static uintptr_t g_cb_callstack[CONFIG_WATCHPOINT_CALLSTACK_DEPTH];
static size_t g_cb_callstack_depth;
#endif

static void reset_callback_state(void)
{
	atomic_set(&g_cb_count, 0);
	g_cb_pc = NULL;
	g_cb_addr = NULL;
	g_cb_addr_valid = false;
	g_cb_flags = 0U;
	g_cb_timing = K_WATCHPOINT_TIMING_UNKNOWN;
	g_cb_rearm_required = false;
	g_cb_cpu = UINT_MAX;
#if defined(CONFIG_WATCHPOINT_CALLSTACK)
	g_cb_callstack_depth = 0U;
#endif
}

static void callback(const struct k_watchpoint *wp,
		     const struct k_watchpoint_event *event, void *arg)
{
	ARG_UNUSED(wp);
	ARG_UNUSED(arg);

	atomic_inc(&g_cb_count);
	g_cb_pc = event->pc;
	g_cb_addr = event->access_addr;
	g_cb_addr_valid = event->access_addr_valid;
	g_cb_flags = event->flags;
	g_cb_timing = event->timing;
	g_cb_rearm_required = event->rearm_required;
	g_cb_cpu = arch_curr_cpu()->id;
#if defined(CONFIG_WATCHPOINT_CALLSTACK)
	g_cb_callstack_depth = MIN(event->callstack_depth,
				   ARRAY_SIZE(g_cb_callstack));
	if (event->callstack == NULL) {
		g_cb_callstack_depth = 0U;
	}
	for (size_t i = 0; i < g_cb_callstack_depth; i++) {
		g_cb_callstack[i] = event->callstack[i];
	}
#endif
}

#if defined(CONFIG_WATCHPOINT_CALLSTACK)
static void print_saved_callstack(void)
{
	printk("watchpoint callstack depth=%zu\n", g_cb_callstack_depth);
	for (size_t i = 0; i < g_cb_callstack_depth; i++) {
		printk("  #%zu %p\n", i, (void *)g_cb_callstack[i]);
	}
}
#endif

static void timing_callback(const struct k_watchpoint *wp,
			    const struct k_watchpoint_event *event, void *arg)
{
	g_timing_seen = g_timing_value;
	callback(wp, event, arg);
}

static void rearm_if_needed(struct k_watchpoint *wp)
{
	if (!k_watchpoint_is_active(wp)) {
		zassert_ok(k_watchpoint_add(wp), "one-shot re-arm failed");
	}
}

static void wp_add_or_skip(struct k_watchpoint *wp)
{
	int ret = k_watchpoint_add(wp);

	if (ret == -ENOTSUP) {
		ztest_test_skip();
	}
	zassert_ok(ret, "k_watchpoint_add failed: %d", ret);
}

ZTEST_SUITE(watchpoint, NULL, NULL, NULL, NULL, NULL);

ZTEST(watchpoint, test_write_fires)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	g_watched = 0U;
	wp_add_or_skip(&wp);

	g_watched = 0xDEADBEEFU;
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_equal(g_cb_flags, K_WATCHPOINT_WRITE);
	zassert_not_null(g_cb_pc);
	zassert_not_equal(g_cb_timing, K_WATCHPOINT_TIMING_UNKNOWN);
	zassert_equal(g_cb_rearm_required,
		      !k_watchpoint_is_active(&wp));
	if (g_cb_addr_valid) {
		zassert_equal(g_cb_addr, (void *)&g_watched);
	}
#if defined(CONFIG_WATCHPOINT_CALLSTACK)
	zassert_true(g_cb_callstack_depth > 1U);
	zassert_equal(g_cb_callstack[0], (uintptr_t)g_cb_pc);
	print_saved_callstack();
#endif

	rearm_if_needed(&wp);
	g_watched = 0xCAFEBABEU;
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_access_and_following_instruction_complete)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	g_watched = 0U;
	g_unwatched = 0U;
	wp_add_or_skip(&wp);

	g_watched = 0x13579BDFU;
	g_unwatched = 0x2468ACE0U;

	zassert_equal(g_watched, 0x13579BDFU);
	zassert_equal(g_unwatched, 0x2468ACE0U);
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_reported_timing_matches_memory_state)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_timing_value,
				   sizeof(g_timing_value),
				   K_WATCHPOINT_WRITE, timing_callback, NULL);
	const uint32_t old_value = 0x12345678U;
	const uint32_t new_value = 0x89ABCDEFU;

	reset_callback_state();
	g_timing_value = old_value;
	g_timing_seen = 0U;
	wp_add_or_skip(&wp);

	g_timing_value = new_value;
	zassert_equal(atomic_get(&g_cb_count), 1);
	if (g_cb_timing == K_WATCHPOINT_TIMING_BEFORE) {
		zassert_equal(g_timing_seen, old_value);
	} else {
		zassert_equal(g_cb_timing, K_WATCHPOINT_TIMING_AFTER);
		zassert_equal(g_timing_seen, new_value);
	}
	zassert_equal(g_timing_value, new_value);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_read_fires)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_bytes[1], 1U,
				   K_WATCHPOINT_READ, callback, NULL);

	reset_callback_state();
	g_bytes[1] = 0x5AU;
	wp_add_or_skip(&wp);

	g_sink = g_bytes[1];
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_equal(g_cb_flags, K_WATCHPOINT_READ);
	zassert_not_null(g_cb_pc);
	if (g_cb_addr_valid) {
		zassert_equal(g_cb_addr, (void *)&g_bytes[1]);
	}

#if defined(CONFIG_RISCV)
	zassert_true(k_watchpoint_is_active(&wp),
		     "watchpoint became one-shot");
	g_sink = g_bytes[1];
	zassert_equal(atomic_get(&g_cb_count), 2);
#endif
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_read_write_fires)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_bytes[2], 1U,
				   K_WATCHPOINT_RW, callback, NULL);

	reset_callback_state();
	g_bytes[2] = 0U;
	wp_add_or_skip(&wp);

	g_bytes[2] = 0xA5U;
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_true((g_cb_flags & K_WATCHPOINT_WRITE) != 0U);

	rearm_if_needed(&wp);
	g_sink = g_bytes[2];
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_true((g_cb_flags & K_WATCHPOINT_READ) != 0U);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_byte_range)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_bytes[1], 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	g_bytes[0] = 0U;
	g_bytes[1] = 0U;
	wp_add_or_skip(&wp);

	g_bytes[0] = 0x11U;
	zassert_equal(atomic_get(&g_cb_count), 0);
	g_bytes[1] = 0x22U;
	zassert_equal(atomic_get(&g_cb_count), 1);

	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_exact_range_or_not_supported)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_bytes[0], 4U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	int ret = k_watchpoint_add(&wp);

	if (ret == -ENOTSUP) {
		return;
	}
	zassert_ok(ret);
	g_bytes[4] = 0x11U;
	zassert_equal(atomic_get(&g_cb_count), 0);
	g_bytes[3] = 0x5AU;
	zassert_equal(atomic_get(&g_cb_count), 1);

	rearm_if_needed(&wp);
	g_bytes[0] = 0xA5U;
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_cross_granule_range_or_not_supported)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_bytes[7], 2U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	int ret = k_watchpoint_add(&wp);

	if (ret == -ENOTSUP) {
		return;
	}
	zassert_ok(ret);
	g_bytes[7] = 0x3CU;
	zassert_equal(atomic_get(&g_cb_count), 1);
	rearm_if_needed(&wp);
	g_bytes[8] = 0xC3U;
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_ok(k_watchpoint_remove(&wp));
}
ZTEST(watchpoint, test_remove_stops)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	wp_add_or_skip(&wp);
	g_watched = 1U;
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_ok(k_watchpoint_remove(&wp));

	g_watched = 2U;
	g_watched = 3U;
	zassert_equal(atomic_get(&g_cb_count), 1);
}

ZTEST(watchpoint, test_unwatched_no_fire)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	wp_add_or_skip(&wp);
	g_unwatched = 0x12345678U;
	zassert_equal(atomic_get(&g_cb_count), 0);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_double_add_busy)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	wp_add_or_skip(&wp);
	zassert_equal(k_watchpoint_add(&wp), -EBUSY);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_remove_idempotent)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	zassert_ok(k_watchpoint_remove(&wp));
	reset_callback_state();
	wp_add_or_skip(&wp);
	g_watched = 99U;
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_ok(k_watchpoint_remove(&wp));
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_descriptor_retarget_after_remove)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_bytes[0], 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	wp.addr = (void *)&g_bytes[0];
	wp_add_or_skip(&wp);
	zassert_ok(k_watchpoint_remove(&wp));

	wp.addr = (void *)&g_bytes[1];
	zassert_ok(k_watchpoint_add(&wp));
	g_bytes[0] = 0x12U;
	zassert_equal(atomic_get(&g_cb_count), 0);
	g_bytes[1] = 0x34U;
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_ok(k_watchpoint_remove(&wp));
}

static void count_callback(const struct k_watchpoint *wp,
			   const struct k_watchpoint_event *event, void *arg)
{
	ARG_UNUSED(wp);
	ARG_UNUSED(event);

	atomic_inc((atomic_t *)arg);
}

ZTEST(watchpoint, test_multiple_watchpoints)
{
	static atomic_t first_count;
	static atomic_t second_count;
	static K_WATCHPOINT_DEFINE(first, (void *)&g_multi[0], 1U,
				   K_WATCHPOINT_WRITE, count_callback,
				   &first_count);
	static K_WATCHPOINT_DEFINE(second, (void *)&g_multi[1], 1U,
				   K_WATCHPOINT_WRITE, count_callback,
				   &second_count);

	atomic_set(&first_count, 0);
	atomic_set(&second_count, 0);
	wp_add_or_skip(&first);

	int ret = k_watchpoint_add(&second);

	if (ret == -ENOSPC || ret == -ENOTSUP) {
		zassert_ok(k_watchpoint_remove(&first));
		return;
	}
	zassert_ok(ret);
	g_multi[0] = 1U;
	g_multi[1] = 2U;
	rearm_if_needed(&first);
	rearm_if_needed(&second);
	g_multi[0] = 3U;

	zassert_equal(atomic_get(&first_count), 2);
	zassert_equal(atomic_get(&second_count), 1);
	zassert_ok(k_watchpoint_remove(&first));
	zassert_ok(k_watchpoint_remove(&second));
}

static volatile uint8_t g_nested_first;
static volatile uint8_t g_nested_second;
static atomic_t g_nested_first_count;
static atomic_t g_nested_second_count;
static atomic_t g_nested_callback_active;
static atomic_t g_nested_reentrant;

static void nested_access_callback(const struct k_watchpoint *wp,
				   const struct k_watchpoint_event *event,
				   void *arg)
{
	ARG_UNUSED(wp);
	ARG_UNUSED(event);
	ARG_UNUSED(arg);

	atomic_inc(&g_nested_first_count);
	atomic_set(&g_nested_callback_active, 1);
	g_nested_second = 0xA5U;
	atomic_set(&g_nested_callback_active, 0);
}

static void nested_second_callback(const struct k_watchpoint *wp,
				   const struct k_watchpoint_event *event,
				   void *arg)
{
	ARG_UNUSED(wp);
	ARG_UNUSED(event);
	ARG_UNUSED(arg);

	if (atomic_get(&g_nested_callback_active) != 0) {
		atomic_set(&g_nested_reentrant, 1);
	}
	atomic_inc(&g_nested_second_count);
}

ZTEST(watchpoint, test_callback_access_does_not_recurse)
{
	static K_WATCHPOINT_DEFINE(first, (void *)&g_nested_first, 1U,
				   K_WATCHPOINT_WRITE,
				   nested_access_callback, NULL);
	static K_WATCHPOINT_DEFINE(second, (void *)&g_nested_second, 1U,
				   K_WATCHPOINT_WRITE,
				   nested_second_callback, NULL);

	atomic_set(&g_nested_first_count, 0);
	atomic_set(&g_nested_second_count, 0);
	atomic_set(&g_nested_callback_active, 0);
	atomic_set(&g_nested_reentrant, 0);
	wp_add_or_skip(&first);

	int ret = k_watchpoint_add(&second);

	if (ret == -ENOSPC || ret == -ENOTSUP) {
		zassert_ok(k_watchpoint_remove(&first));
		return;
	}
	zassert_ok(ret);

	zassert_equal(atomic_get(&g_nested_second_count), 0);
	g_nested_first = 0x5AU;
	zassert_equal(atomic_get(&g_nested_first_count), 1);
	zassert_equal(atomic_get(&g_nested_reentrant), 0,
		      "second callback was recursive");

	int second_count = atomic_get(&g_nested_second_count);

	zassert_true(second_count == 0 || second_count == 1);
	rearm_if_needed(&second);
	g_nested_second = 0x3CU;
	zassert_equal(atomic_get(&g_nested_second_count), second_count + 1);
	zassert_ok(k_watchpoint_remove(&first));
	zassert_ok(k_watchpoint_remove(&second));
}

ZTEST(watchpoint, test_invalid_args)
{
	static K_WATCHPOINT_DEFINE(zero_size, (void *)&g_watched, 0U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(no_flag, (void *)&g_watched, 1U,
				   0U, callback, NULL);
	static K_WATCHPOINT_DEFINE(bad_flags, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE | BIT(8), callback, NULL);
	static K_WATCHPOINT_DEFINE(no_callback, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, NULL, NULL);
	static K_WATCHPOINT_DEFINE(overflow, (void *)UINTPTR_MAX, 2U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	zassert_equal(k_watchpoint_add(NULL), -EINVAL);
	zassert_equal(k_watchpoint_add(&zero_size), -EINVAL);
	zassert_equal(k_watchpoint_add(&no_flag), -EINVAL);
	zassert_equal(k_watchpoint_add(&bad_flags), -EINVAL);
	zassert_equal(k_watchpoint_add(&no_callback), -EINVAL);
	zassert_equal(k_watchpoint_add(&overflow), -EINVAL);
	zassert_equal(k_watchpoint_remove(NULL), -EINVAL);
	zassert_false(k_watchpoint_is_active(NULL));
}

ZTEST(watchpoint, test_zero_address_is_valid)
{
	static K_WATCHPOINT_DEFINE(wp, NULL, 1U, K_WATCHPOINT_WRITE,
				   callback, NULL);
	int ret = k_watchpoint_add(&wp);

	if (ret == -ENOTSUP) {
		return;
	}
	zassert_ok(ret);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_zero_initialized_descriptor)
{
	static struct k_watchpoint wp = {
		.addr = (void *)&g_watched,
		.size = sizeof(g_watched),
		.flags = K_WATCHPOINT_WRITE,
		.cb = callback,
	};

	int ret = k_watchpoint_add(&wp);

	if (ret == -ENOTSUP) {
		return;
	}
	zassert_ok(ret);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_ok(k_watchpoint_remove(&wp));
}

static atomic_t g_callback_add_ret;
static atomic_t g_callback_remove_ret;
static atomic_t g_callback_reject_count;
static K_WATCHPOINT_DEFINE(g_callback_nested_wp, (void *)&g_unwatched, 1U,
			   K_WATCHPOINT_WRITE, callback, NULL);

static void api_rejecting_callback(const struct k_watchpoint *wp,
				   const struct k_watchpoint_event *event,
				   void *arg)
{
	ARG_UNUSED(event);
	ARG_UNUSED(arg);

	atomic_inc(&g_callback_reject_count);
	atomic_set(&g_callback_add_ret,
		   k_watchpoint_add(&g_callback_nested_wp));
	atomic_set(&g_callback_remove_ret,
		   k_watchpoint_remove((struct k_watchpoint *)wp));
}

ZTEST(watchpoint, test_callback_context_rejected)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE,
				   api_rejecting_callback, NULL);

	atomic_set(&g_callback_nested_wp._state, 0);
	atomic_set(&g_callback_add_ret, 0);
	atomic_set(&g_callback_remove_ret, 0);
	atomic_set(&g_callback_reject_count, 0);
	wp_add_or_skip(&wp);
	g_watched = 0x10203040U;

	int add_ret = atomic_get(&g_callback_add_ret);
	int remove_ret = atomic_get(&g_callback_remove_ret);

	if (k_watchpoint_is_active(&g_callback_nested_wp)) {
		zassert_ok(k_watchpoint_remove(&g_callback_nested_wp));
	}
	zassert_equal(atomic_get(&g_callback_reject_count), 1,
		      "callback did not run");
	zassert_equal(add_ret, -EWOULDBLOCK,
		      "callback add returned %d", add_ret);
	zassert_equal(remove_ret, -EWOULDBLOCK,
		      "callback remove returned %d", remove_ret);
	zassert_ok(k_watchpoint_remove(&wp));
}

#if defined(CONFIG_DEBUGPOINT)
#define RESOURCE_TEST_COUNT (CONFIG_DEBUGPOINT_MAX_SLOTS + 1)
#else
#define RESOURCE_TEST_COUNT 1
#endif

ZTEST(watchpoint, test_resource_exhaustion_and_reuse)
{
	static volatile uint8_t targets[RESOURCE_TEST_COUNT]
		__aligned(16);
	struct k_watchpoint watchpoints[RESOURCE_TEST_COUNT];
	static atomic_t unexpected_hits;
	int armed = 0;
	int failure = 0;

	atomic_set(&unexpected_hits, 0);
	for (int i = 0; i < ARRAY_SIZE(watchpoints); i++) {
		watchpoints[i] = (struct k_watchpoint)
			K_WATCHPOINT_INITIALIZER((void *)&targets[i], 1U,
						 K_WATCHPOINT_WRITE,
						 count_callback,
						 &unexpected_hits);
		int ret = k_watchpoint_add(&watchpoints[i]);

		if (ret != 0) {
			failure = ret;
			break;
		}
		armed++;
	}

	if (armed == 0 && failure == -ENOTSUP) {
		ztest_test_skip();
	}
	zassert_true(armed > 0);
	zassert_true(failure == -ENOSPC || failure == -ENOTSUP);

	zassert_ok(k_watchpoint_remove(&watchpoints[0]));
	zassert_ok(k_watchpoint_add(&watchpoints[armed]));
	zassert_ok(k_watchpoint_remove(&watchpoints[armed]));

	for (int i = 1; i < armed; i++) {
		zassert_ok(k_watchpoint_remove(&watchpoints[i]));
	}
	zassert_equal(atomic_get(&unexpected_hits), 0);
}
#if defined(CONFIG_RISCV) && defined(CONFIG_DEBUGPOINT)
extern int z_riscv_debugpoint_handle(struct arch_esf *esf);

static volatile uint64_t g_riscv_scalar_target __aligned(8);

#if defined(CONFIG_RISCV_ISA_EXT_ZAAMO)
static volatile uint32_t g_riscv_amo32 __aligned(4);
#if __riscv_xlen >= 64
static volatile uint64_t g_riscv_amo64 __aligned(8);
#endif
#endif

#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_F)
static volatile uint32_t g_riscv_fp_source32 __aligned(4);
static volatile uint32_t g_riscv_fp_target32 __aligned(4);
static volatile uint32_t g_riscv_fp_sink32 __aligned(4);
#if defined(CONFIG_RISCV_ISA_EXT_D)
static volatile uint64_t g_riscv_fp_source64 __aligned(8);
static volatile uint64_t g_riscv_fp_target64 __aligned(8);
static volatile uint64_t g_riscv_fp_sink64 __aligned(8);
#endif
#endif

__attribute__((noinline))
static void z_test_watchpoint_scalar_stores(
	volatile uint64_t *addr, uintptr_t value)
{
#if __riscv_xlen >= 64
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"sb %1, 0(%0)\n"
		"sh %1, 0(%0)\n"
		"sw %1, 0(%0)\n"
		"sd %1, 0(%0)\n"
		".option pop\n"
		:
		: "r"(addr), "r"(value)
		: "memory");
#else
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"sb %1, 0(%0)\n"
		"sh %1, 0(%0)\n"
		"sw %1, 0(%0)\n"
		".option pop\n"
		:
		: "r"(addr), "r"(value)
		: "memory");
#endif
}

__attribute__((noinline))
static uintptr_t z_test_watchpoint_scalar_loads(volatile uint64_t *addr)
{
	uintptr_t value;

#if __riscv_xlen >= 64
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"lb %0, 0(%1)\n"
		"lbu %0, 0(%1)\n"
		"lh %0, 0(%1)\n"
		"lhu %0, 0(%1)\n"
		"lw %0, 0(%1)\n"
		"lwu %0, 0(%1)\n"
		"ld %0, 0(%1)\n"
		".option pop\n"
		: "=&r"(value)
		: "r"(addr)
		: "memory");
#else
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"lb %0, 0(%1)\n"
		"lbu %0, 0(%1)\n"
		"lh %0, 0(%1)\n"
		"lhu %0, 0(%1)\n"
		"lw %0, 0(%1)\n"
		".option pop\n"
		: "=&r"(value)
		: "r"(addr)
		: "memory");
#endif
	return value;
}

#if defined(CONFIG_RISCV_ISA_EXT_ZAAMO)
__attribute__((noinline))
static uint32_t z_test_watchpoint_amo32_all(
	volatile uint32_t *addr, uint32_t value)
{
	uintptr_t old;

	__asm__ volatile(
		"amoadd.w.aqrl %0, %2, (%1)\n"
		"amoswap.w %0, %2, (%1)\n"
		"amoxor.w %0, %2, (%1)\n"
		"amoor.w %0, %2, (%1)\n"
		"amoand.w %0, %2, (%1)\n"
		"amomin.w %0, %2, (%1)\n"
		"amomax.w %0, %2, (%1)\n"
		"amominu.w %0, %2, (%1)\n"
		"amomaxu.w %0, %2, (%1)\n"
		: "=&r"(old)
		: "r"(addr), "r"((uintptr_t)value)
		: "memory");
	return (uint32_t)old;
}

#if defined(CONFIG_SMP)
__attribute__((noinline))
static uint32_t z_test_watchpoint_amoadd32(
	volatile uint32_t *addr, uint32_t value)
{
	uintptr_t old;

	__asm__ volatile("amoadd.w.aqrl %0, %2, (%1)"
			 : "=&r"(old)
			 : "r"(addr), "r"((uintptr_t)value)
			 : "memory");
	return (uint32_t)old;
}
#endif

#if __riscv_xlen >= 64
__attribute__((noinline))
static uint64_t z_test_watchpoint_amoadd64(
	volatile uint64_t *addr, uint64_t value)
{
	uint64_t old;

	__asm__ volatile("amoadd.d.aqrl %0, %2, (%1)"
			 : "=&r"(old)
			 : "r"(addr), "r"(value)
			 : "memory");
	return old;
}
#endif
#endif


#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_F)
__attribute__((noinline))
static void z_test_watchpoint_fp_store32(
	volatile void *target, volatile void *source)
{
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"flw ft0, 0(%1)\n"
		"fsw ft0, 0(%0)\n"
		".option pop\n"
		:
		: "r"(target), "r"(source)
		: "ft0", "memory");
}

__attribute__((noinline))
static void z_test_watchpoint_fp_load32(
	volatile void *source, volatile void *sink)
{
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"flw ft0, 0(%0)\n"
		"fsw ft0, 0(%1)\n"
		".option pop\n"
		:
		: "r"(source), "r"(sink)
		: "ft0", "memory");
}

#if defined(CONFIG_RISCV_ISA_EXT_D)
__attribute__((noinline))
static void z_test_watchpoint_fp_store64(
	volatile void *target, volatile void *source)
{
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"fld ft0, 0(%1)\n"
		"fsd ft0, 0(%0)\n"
		".option pop\n"
		:
		: "r"(target), "r"(source)
		: "ft0", "memory");
}

__attribute__((noinline))
static void z_test_watchpoint_fp_load64(
	volatile void *source, volatile void *sink)
{
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"fld ft0, 0(%0)\n"
		"fsd ft0, 0(%1)\n"
		".option pop\n"
		:
		: "r"(source), "r"(sink)
		: "ft0", "memory");
}
#endif
#endif

#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_ZCF)
__attribute__((noinline))
static void z_test_watchpoint_c_fp_store32(
	volatile void *target, volatile void *source)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)target;

	__asm__ volatile(
		"flw fa0, 0(%1)\n"
		".option push\n"
		".option rvc\n"
		"c.fsw fa0, 0(a0)\n"
		".option pop\n"
		:
		: "r"(base), "r"(source)
		: "fa0", "memory");
}

__attribute__((noinline))
static void z_test_watchpoint_c_fp_load32(
	volatile void *source, volatile void *sink)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)source;

	__asm__ volatile(
		".option push\n"
		".option rvc\n"
		"c.flw fa0, 0(a0)\n"
		".option pop\n"
		"fsw fa0, 0(%1)\n"
		:
		: "r"(base), "r"(sink)
		: "fa0", "memory");
}
#endif

#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_ZCD)
__attribute__((noinline))
static void z_test_watchpoint_c_fp_store64(
	volatile void *target, volatile void *source)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)target;

	__asm__ volatile(
		"fld fa0, 0(%1)\n"
		".option push\n"
		".option rvc\n"
		"c.fsd fa0, 0(a0)\n"
		".option pop\n"
		:
		: "r"(base), "r"(source)
		: "fa0", "memory");
}

__attribute__((noinline))
static void z_test_watchpoint_c_fp_load64(
	volatile void *source, volatile void *sink)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)source;

	__asm__ volatile(
		".option push\n"
		".option rvc\n"
		"c.fld fa0, 0(a0)\n"
		".option pop\n"
		"fsd fa0, 0(%1)\n"
		:
		: "r"(base), "r"(sink)
		: "fa0", "memory");
}
#endif

__attribute__((noinline))
static void z_test_watchpoint_store32(volatile uint32_t *addr, uint32_t value)
{
	__asm__ volatile(
		".option push\n"
		".option norvc\n"
		"sw %1, 0(%0)\n"
		".option pop\n"
		:
		: "r"(addr), "r"(value)
		: "memory");
}

#if defined(CONFIG_RISCV_ISA_EXT_ZCA)
__attribute__((noinline))
static void z_test_watchpoint_c_store(volatile uint32_t *addr, uint32_t value)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)addr;
	register uint32_t data __asm__("a1") = value;

	__asm__ volatile(
		".option push\n"
		".option rvc\n"
		"c.sw a1, 0(a0)\n"
		".option pop\n"
		:
		: "r"(base), "r"(data)
		: "memory");
}

#if __riscv_xlen < 64
__attribute__((noinline))
static uint32_t z_test_watchpoint_c_load(volatile uint32_t *addr)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)addr;
	register uint32_t data __asm__("a1");

	__asm__ volatile(
		".option push\n"
		".option rvc\n"
		"c.lw %0, 0(%1)\n"
		".option pop\n"
		: "=r"(data)
		: "r"(base)
		: "memory");
	return data;
}

#endif

#if __riscv_xlen >= 64
__attribute__((noinline))
static void z_test_watchpoint_c_store64(
	volatile uint64_t *addr, uint64_t value)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)addr;
	register uint64_t data __asm__("a1") = value;

	__asm__ volatile(
		".option push\n"
		".option rvc\n"
		"c.sd a1, 0(a0)\n"
		".option pop\n"
		:
		: "r"(base), "r"(data)
		: "memory");
}

__attribute__((noinline))
static uint64_t z_test_watchpoint_c_load64(volatile uint64_t *addr)
{
	register uintptr_t base __asm__("a0") = (uintptr_t)addr;
	register uint64_t data __asm__("a1");

	__asm__ volatile(
		".option push\n"
		".option rvc\n"
		"c.ld %0, 0(%1)\n"
		".option pop\n"
		: "=r"(data)
		: "r"(base)
		: "memory");
	return data;
}
#endif
#endif

static const uint16_t g_ebreak32[] __aligned(2) = {0x0073U, 0x0010U};
static const uint16_t g_ebreak16 __aligned(2) = 0x9002U;

ZTEST(watchpoint, test_riscv_unrepresentable_ranges)
{
	static K_WATCHPOINT_DEFINE(non_power_of_two, (void *)&g_bytes[0], 3U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(unaligned, (void *)&g_bytes[1], 2U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	zassert_equal(k_watchpoint_add(&non_power_of_two), -ENOTSUP);
	zassert_equal(k_watchpoint_add(&unaligned), -ENOTSUP);
}

static void verify_riscv_store_persistence(
	void (*store)(volatile uint32_t *addr, uint32_t value),
	struct k_watchpoint *wp)
{
	reset_callback_state();
	g_watched = 0U;
	wp_add_or_skip(wp);

	store(&g_watched, 0x11223344U);
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_not_equal(g_cb_timing, K_WATCHPOINT_TIMING_UNKNOWN);
	zassert_true(k_watchpoint_is_active(wp),
		     "watchpoint became one-shot");

	store(&g_watched, 0x55667788U);
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_equal(g_watched, 0x55667788U);
	zassert_ok(k_watchpoint_remove(wp));
}

ZTEST(watchpoint, test_riscv_persistent_32bit_store)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	verify_riscv_store_persistence(z_test_watchpoint_store32, &wp);
}

#if defined(CONFIG_RISCV_ISA_EXT_ZCA)
ZTEST(watchpoint, test_riscv_persistent_compressed_store)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	verify_riscv_store_persistence(z_test_watchpoint_c_store, &wp);
}
#endif


ZTEST(watchpoint, test_riscv_back_to_back_scalar_stores)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_riscv_scalar_target, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	const int stores_per_call = __riscv_xlen >= 64 ? 4 : 3;

	reset_callback_state();
	g_riscv_scalar_target = 0U;
	wp_add_or_skip(&wp);

	z_test_watchpoint_scalar_stores(&g_riscv_scalar_target, 0x5aU);
	zassert_equal(atomic_get(&g_cb_count), stores_per_call);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_false(g_cb_rearm_required);

	z_test_watchpoint_scalar_stores(&g_riscv_scalar_target, 0xa5U);
	zassert_equal(atomic_get(&g_cb_count), 2 * stores_per_call);
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_riscv_back_to_back_scalar_loads)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_riscv_scalar_target, 1U,
				   K_WATCHPOINT_READ, callback, NULL);
	const int loads_per_call = __riscv_xlen >= 64 ? 7 : 5;

	reset_callback_state();
	g_riscv_scalar_target = UINT64_C(0x1122334455667788);
	wp_add_or_skip(&wp);

	(void)z_test_watchpoint_scalar_loads(&g_riscv_scalar_target);
	zassert_equal(atomic_get(&g_cb_count), loads_per_call);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_false(g_cb_rearm_required);

	(void)z_test_watchpoint_scalar_loads(&g_riscv_scalar_target);
	zassert_equal(atomic_get(&g_cb_count), 2 * loads_per_call);
	zassert_ok(k_watchpoint_remove(&wp));
}

#if defined(CONFIG_RISCV_ISA_EXT_ZCA)
#if __riscv_xlen < 64
ZTEST(watchpoint, test_riscv_persistent_compressed_load)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_READ, callback, NULL);

	reset_callback_state();
	g_watched = 0x12345678U;
	wp_add_or_skip(&wp);

	zassert_equal(z_test_watchpoint_c_load(&g_watched), 0x12345678U);
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_equal(z_test_watchpoint_c_load(&g_watched), 0x12345678U);
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_ok(k_watchpoint_remove(&wp));
}

#endif

#if __riscv_xlen >= 64
ZTEST(watchpoint, test_riscv_persistent_compressed_64bit_accesses)
{
	static K_WATCHPOINT_DEFINE(write_wp,
				   (void *)&g_riscv_scalar_target, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(read_wp,
				   (void *)&g_riscv_scalar_target, 1U,
				   K_WATCHPOINT_READ, callback, NULL);

	reset_callback_state();
	g_riscv_scalar_target = 0U;
	wp_add_or_skip(&write_wp);
	z_test_watchpoint_c_store64(&g_riscv_scalar_target,
				    UINT64_C(0x1122334455667788));
	z_test_watchpoint_c_store64(&g_riscv_scalar_target,
				    UINT64_C(0x8877665544332211));
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_true(k_watchpoint_is_active(&write_wp));
	zassert_ok(k_watchpoint_remove(&write_wp));

	reset_callback_state();
	wp_add_or_skip(&read_wp);
	zassert_equal(z_test_watchpoint_c_load64(&g_riscv_scalar_target),
		      UINT64_C(0x8877665544332211));
	zassert_equal(z_test_watchpoint_c_load64(&g_riscv_scalar_target),
		      UINT64_C(0x8877665544332211));
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_true(k_watchpoint_is_active(&read_wp));
	zassert_ok(k_watchpoint_remove(&read_wp));
}
#endif
#endif

#if defined(CONFIG_RISCV_ISA_EXT_ZAAMO)
ZTEST(watchpoint, test_riscv_persistent_zaamo_word_operations)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_riscv_amo32, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	g_riscv_amo32 = 0x10U;
	wp_add_or_skip(&wp);

	(void)z_test_watchpoint_amo32_all(&g_riscv_amo32, 0x5aU);
	zassert_equal(atomic_get(&g_cb_count), 9);
	zassert_equal(g_riscv_amo32, 0x5aU);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_false(g_cb_rearm_required);

	(void)z_test_watchpoint_amo32_all(&g_riscv_amo32, 0x5aU);
	zassert_equal(atomic_get(&g_cb_count), 18);
	zassert_equal(g_riscv_amo32, 0x5aU);
	zassert_ok(k_watchpoint_remove(&wp));
}

#if __riscv_xlen >= 64
ZTEST(watchpoint, test_riscv_persistent_zaamo_doubleword)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_riscv_amo64, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	g_riscv_amo64 = 3U;
	wp_add_or_skip(&wp);
	(void)z_test_watchpoint_amoadd64(&g_riscv_amo64, 4U);
	(void)z_test_watchpoint_amoadd64(&g_riscv_amo64, 4U);
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_equal(g_riscv_amo64, 11U);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_ok(k_watchpoint_remove(&wp));
}
#endif
#endif


#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_F)
typedef void (*riscv_fp_access_t)(volatile void *first,
				  volatile void *second);

static void verify_riscv_fp_persistence(
	struct k_watchpoint *write_wp, struct k_watchpoint *read_wp,
	riscv_fp_access_t store, riscv_fp_access_t load,
	volatile void *target, volatile void *source, volatile void *sink)
{
	reset_callback_state();
	wp_add_or_skip(write_wp);
	store(target, source);
	store(target, source);
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_true(k_watchpoint_is_active(write_wp));
	zassert_false(g_cb_rearm_required);
	zassert_ok(k_watchpoint_remove(write_wp));

	reset_callback_state();
	wp_add_or_skip(read_wp);
	load(target, sink);
	load(target, sink);
	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_true(k_watchpoint_is_active(read_wp));
	zassert_false(g_cb_rearm_required);
	zassert_ok(k_watchpoint_remove(read_wp));
}
ZTEST(watchpoint, test_riscv_persistent_fp32_accesses)
{
	static K_WATCHPOINT_DEFINE(write_wp, (void *)&g_riscv_fp_target32,
				   1U, K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(read_wp, (void *)&g_riscv_fp_target32,
				   1U, K_WATCHPOINT_READ, callback, NULL);

	g_riscv_fp_source32 = 0x3f800000U;
	g_riscv_fp_target32 = 0U;
	g_riscv_fp_sink32 = 0U;
	verify_riscv_fp_persistence(
		&write_wp, &read_wp, z_test_watchpoint_fp_store32,
		z_test_watchpoint_fp_load32, &g_riscv_fp_target32,
		&g_riscv_fp_source32, &g_riscv_fp_sink32);
	zassert_equal(g_riscv_fp_target32, g_riscv_fp_source32);
	zassert_equal(g_riscv_fp_sink32, g_riscv_fp_target32);
}

#if defined(CONFIG_RISCV_ISA_EXT_D)
ZTEST(watchpoint, test_riscv_persistent_fp64_accesses)
{
	static K_WATCHPOINT_DEFINE(write_wp, (void *)&g_riscv_fp_target64,
				   1U, K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(read_wp, (void *)&g_riscv_fp_target64,
				   1U, K_WATCHPOINT_READ, callback, NULL);

	g_riscv_fp_source64 = UINT64_C(0x3ff0000000000000);
	g_riscv_fp_target64 = 0U;
	g_riscv_fp_sink64 = 0U;
	verify_riscv_fp_persistence(
		&write_wp, &read_wp, z_test_watchpoint_fp_store64,
		z_test_watchpoint_fp_load64, &g_riscv_fp_target64,
		&g_riscv_fp_source64, &g_riscv_fp_sink64);
	zassert_equal(g_riscv_fp_target64, g_riscv_fp_source64);
	zassert_equal(g_riscv_fp_sink64, g_riscv_fp_target64);
}
#endif
#endif

#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_ZCF)
ZTEST(watchpoint, test_riscv_persistent_compressed_fp32_accesses)
{
	static K_WATCHPOINT_DEFINE(write_wp, (void *)&g_riscv_fp_target32,
				   1U, K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(read_wp, (void *)&g_riscv_fp_target32,
				   1U, K_WATCHPOINT_READ, callback, NULL);

	g_riscv_fp_source32 = 0x3f800000U;
	g_riscv_fp_target32 = 0U;
	g_riscv_fp_sink32 = 0U;
	verify_riscv_fp_persistence(
		&write_wp, &read_wp, z_test_watchpoint_c_fp_store32,
		z_test_watchpoint_c_fp_load32, &g_riscv_fp_target32,
		&g_riscv_fp_source32, &g_riscv_fp_sink32);
	zassert_equal(g_riscv_fp_target32, g_riscv_fp_source32);
	zassert_equal(g_riscv_fp_sink32, g_riscv_fp_target32);
}
#endif

#if defined(CONFIG_FPU) && defined(CONFIG_RISCV_ISA_EXT_ZCD)
ZTEST(watchpoint, test_riscv_persistent_compressed_fp64_accesses)
{
	static K_WATCHPOINT_DEFINE(write_wp, (void *)&g_riscv_fp_target64,
				   1U, K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(read_wp, (void *)&g_riscv_fp_target64,
				   1U, K_WATCHPOINT_READ, callback, NULL);

	g_riscv_fp_source64 = UINT64_C(0x3ff0000000000000);
	g_riscv_fp_target64 = 0U;
	g_riscv_fp_sink64 = 0U;
	verify_riscv_fp_persistence(
		&write_wp, &read_wp, z_test_watchpoint_c_fp_store64,
		z_test_watchpoint_c_fp_load64, &g_riscv_fp_target64,
		&g_riscv_fp_source64, &g_riscv_fp_sink64);
	zassert_equal(g_riscv_fp_target64, g_riscv_fp_source64);
	zassert_equal(g_riscv_fp_sink64, g_riscv_fp_target64);
}
#endif

ZTEST(watchpoint, test_riscv_overlap_rejected)
{
	static K_WATCHPOINT_DEFINE(first, (void *)&g_bytes[0], 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(second, (void *)&g_bytes[0], 1U,
				   K_WATCHPOINT_READ, callback, NULL);

	wp_add_or_skip(&first);
	zassert_equal(k_watchpoint_add(&second), -ENOTSUP);
	zassert_ok(k_watchpoint_remove(&first));
}

ZTEST(watchpoint, test_riscv_ebreak_not_consumed)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(zero_wp, NULL, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	struct arch_esf esf = {0};

	wp_add_or_skip(&wp);
	esf.mepc = (uintptr_t)g_ebreak32;
	zassert_equal(z_riscv_debugpoint_handle(&esf), -ENOENT);
	esf.mepc = (uintptr_t)&g_ebreak16;
	zassert_equal(z_riscv_debugpoint_handle(&esf), -ENOENT);
	zassert_ok(k_watchpoint_remove(&wp));

	wp_add_or_skip(&zero_wp);
	esf.mepc = (uintptr_t)g_ebreak32;
	zassert_equal(z_riscv_debugpoint_handle(&esf), -ENOENT);
	esf.mepc = UINTPTR_MAX - 1U;
	zassert_equal(z_riscv_debugpoint_handle(&esf), -ENOENT);
	zassert_ok(k_watchpoint_remove(&zero_wp));
}
#endif

static K_SEM_DEFINE(g_isr_done, 0, 1);
static int g_isr_add_ret;
static int g_isr_remove_ret;
static K_WATCHPOINT_DEFINE(g_isr_wp, (void *)&g_watched, 1U,
			   K_WATCHPOINT_WRITE, callback, NULL);
static struct k_watchpoint g_fake_active_wp =
	K_WATCHPOINT_INITIALIZER((void *)&g_watched, 1U,
				 K_WATCHPOINT_WRITE, callback, NULL);

static void isr_context_timer(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	g_isr_add_ret = k_watchpoint_add(&g_isr_wp);
	g_isr_remove_ret = k_watchpoint_remove(&g_fake_active_wp);
	k_sem_give(&g_isr_done);
}

K_TIMER_DEFINE(g_isr_timer, isr_context_timer, NULL);

static K_SEM_DEFINE(g_isr_write_done, 0, 1);

static void isr_write_timer(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	g_watched = 0x76543210U;
	k_sem_give(&g_isr_write_done);
}

K_TIMER_DEFINE(g_isr_write_timer, isr_write_timer, NULL);

ZTEST(watchpoint, test_hit_from_isr)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	k_sem_reset(&g_isr_write_done);
	wp_add_or_skip(&wp);
	k_timer_start(&g_isr_write_timer, K_NO_WAIT, K_NO_WAIT);

	zassert_ok(k_sem_take(&g_isr_write_done, K_MSEC(500)));
	zexpect_equal(atomic_get(&g_cb_count), 1);
	zexpect_equal(g_watched, 0x76543210U);
	zexpect_not_null(g_cb_pc);
	zexpect_equal(g_cb_rearm_required,
		      !k_watchpoint_is_active(&wp));
#if defined(CONFIG_RISCV)
	if (g_cb_timing == K_WATCHPOINT_TIMING_BEFORE &&
	    !IS_ENABLED(CONFIG_RISCV_HAS_TCONTROL)) {
		zexpect_true(g_cb_rearm_required);
	}
#endif
#if defined(CONFIG_WATCHPOINT_CALLSTACK)
	zexpect_true(g_cb_callstack_depth > 0U);
	if (g_cb_callstack_depth > 0U) {
		zexpect_equal(g_cb_callstack[0], (uintptr_t)g_cb_pc);
	}
#endif
	zassert_ok(k_watchpoint_remove(&wp));
}

ZTEST(watchpoint, test_isr_context_rejected)
{
	atomic_set(&g_isr_wp._state, 0);
	g_isr_add_ret = 0;
	g_isr_remove_ret = 0;

	k_timer_start(&g_isr_timer, K_NO_WAIT, K_NO_WAIT);
	zassert_ok(k_sem_take(&g_isr_done, K_MSEC(500)));
	zassert_equal(g_isr_add_ret, -EWOULDBLOCK);
	zassert_equal(g_isr_remove_ret, -EWOULDBLOCK);
}

ZTEST(watchpoint, test_irq_locked_context_rejected)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static struct k_watchpoint fake_active =
		K_WATCHPOINT_INITIALIZER((void *)&g_watched, 1U,
				 K_WATCHPOINT_WRITE, callback, NULL);

	atomic_set(&wp._state, 0);
	unsigned int key = irq_lock();
	int add_ret = k_watchpoint_add(&wp);
	int remove_ret = k_watchpoint_remove(&fake_active);

	irq_unlock(key);
	zassert_equal(add_ret, -EWOULDBLOCK);
	zassert_equal(remove_ret, -EWOULDBLOCK);
}

#if defined(CONFIG_USERSPACE)
static void user_writer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	g_user_watched = 0xA55AA55AU;
}

ZTEST(watchpoint, test_user_mode_write_fires)
{
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_user_watched,
				   sizeof(g_user_watched),
				   K_WATCHPOINT_WRITE, callback, NULL);
	struct k_mem_partition *partitions[] = {
		&watchpoint_user_partition,
	};

	k_mem_domain_init(&g_user_domain, ARRAY_SIZE(partitions), partitions);
	reset_callback_state();
	g_user_watched = 0U;
	wp_add_or_skip(&wp);

	k_tid_t tid = k_thread_create(&g_user_thread, g_user_stack,
				      USER_STACK_SIZE, user_writer_fn,
				      NULL, NULL, NULL, K_PRIO_PREEMPT(7),
				      K_USER, K_FOREVER);

	k_mem_domain_add_thread(&g_user_domain, tid);
	k_thread_start(tid);
	zassert_ok(k_thread_join(tid, K_MSEC(500)));
	zexpect_equal(g_user_watched, 0xA55AA55AU);
	zexpect_equal(atomic_get(&g_cb_count), 1);
#if defined(CONFIG_WATCHPOINT_CALLSTACK)
	zexpect_true(g_cb_callstack_depth > 1U);
#endif
	zassert_ok(k_watchpoint_remove(&wp));
}
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
#define SMP_STACK_SIZE 3072

static K_THREAD_STACK_DEFINE(g_writer_stack, SMP_STACK_SIZE);
static K_THREAD_STACK_DEFINE(g_remover_stack, SMP_STACK_SIZE);
static K_THREAD_STACK_DEFINE(g_adder_stack, SMP_STACK_SIZE);
static struct k_thread g_writer_thread;
static struct k_thread g_remover_thread;
static struct k_thread g_adder_thread;

static void writer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	*(volatile uint32_t *)p1 = 0xABCD1234U;
}

#if defined(CONFIG_RISCV)
static void writer_twice_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	volatile uint32_t *target = p1;

	*target = 0x12345678U;
	*target = 0x87654321U;
}
#endif

#if defined(CONFIG_RISCV) && defined(CONFIG_RISCV_ISA_EXT_ZAAMO)
struct smp_amo_writer_request {
	volatile uint32_t *target;
	struct k_sem *ready;
	struct k_sem *start;
	int iterations;
	int ret;
};

static void amo_writer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct smp_amo_writer_request *request = p1;

	k_sem_give(request->ready);
	request->ret = k_sem_take(request->start, K_MSEC(500));
	if (request->ret != 0) {
		return;
	}

	for (int i = 0; i < request->iterations; i++) {
		(void)z_test_watchpoint_amoadd32(request->target, 1U);
	}
}
#endif

static int run_on_cpu(k_thread_entry_t entry, unsigned int cpu,
		      void *arg)
{
	k_tid_t tid = k_thread_create(&g_writer_thread, g_writer_stack,
				      SMP_STACK_SIZE, entry, arg, NULL, NULL,
				      K_PRIO_PREEMPT(7), 0, K_FOREVER);

	k_thread_cpu_pin(tid, cpu);
	k_thread_start(tid);
	return k_thread_join(tid, K_MSEC(500));
}

ZTEST(watchpoint, test_smp_fires_on_remote_cpu)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);

	reset_callback_state();
	wp_add_or_skip(&wp);

	unsigned int remote_cpu =
		(arch_curr_cpu()->id + 1U) % arch_num_cpus();
	k_tid_t tid = k_thread_create(&g_writer_thread, g_writer_stack,
				      SMP_STACK_SIZE, writer_fn,
				      (void *)&g_watched, NULL, NULL,
				      K_PRIO_PREEMPT(7), 0, K_FOREVER);

	k_thread_cpu_pin(tid, remote_cpu);
	k_thread_start(tid);
	zassert_ok(k_thread_join(&g_writer_thread, K_MSEC(500)));

	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_equal(g_cb_cpu, remote_cpu);
	zassert_ok(k_watchpoint_remove(&wp));
}

#if defined(CONFIG_RISCV)
ZTEST(watchpoint, test_riscv_smp_persistent_on_remote_cpu)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	unsigned int remote_cpu =
		(arch_curr_cpu()->id + 1U) % arch_num_cpus();

	reset_callback_state();
	g_watched = 0U;
	wp_add_or_skip(&wp);
	zassert_ok(run_on_cpu(writer_twice_fn, remote_cpu,
			      (void *)&g_watched));

	zassert_equal(atomic_get(&g_cb_count), 2);
	zassert_equal(g_cb_cpu, remote_cpu);
	zassert_equal(g_watched, 0x87654321U);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_ok(k_watchpoint_remove(&wp));
}

#if defined(CONFIG_RISCV_ISA_EXT_ZAAMO)
ZTEST(watchpoint, test_riscv_smp_concurrent_amo_hits)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	static atomic_t hit_count;
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_riscv_amo32, 1U,
				   K_WATCHPOINT_WRITE, count_callback,
				   &hit_count);
	struct k_sem ready;
	struct k_sem start;
	struct smp_amo_writer_request first = {
		.target = &g_riscv_amo32,
		.ready = &ready,
		.start = &start,
		.iterations = 4,
		.ret = -EINPROGRESS,
	};
	struct smp_amo_writer_request second = {
		.target = &g_riscv_amo32,
		.ready = &ready,
		.start = &start,
		.iterations = 4,
		.ret = -EINPROGRESS,
	};
	unsigned int local_cpu = arch_curr_cpu()->id;
	unsigned int remote_cpu = (local_cpu + 1U) % arch_num_cpus();

	atomic_set(&hit_count, 0);
	g_riscv_amo32 = 0U;
	k_sem_init(&ready, 0, 2);
	k_sem_init(&start, 0, 2);
	wp_add_or_skip(&wp);

	k_tid_t first_tid = k_thread_create(
		&g_remover_thread, g_remover_stack, SMP_STACK_SIZE,
		amo_writer_fn, &first, NULL, NULL, K_PRIO_PREEMPT(7),
		0, K_FOREVER);
	k_tid_t second_tid = k_thread_create(
		&g_adder_thread, g_adder_stack, SMP_STACK_SIZE,
		amo_writer_fn, &second, NULL, NULL, K_PRIO_PREEMPT(7),
		0, K_FOREVER);

	k_thread_cpu_pin(first_tid, local_cpu);
	k_thread_cpu_pin(second_tid, remote_cpu);
	k_thread_start(first_tid);
	k_thread_start(second_tid);
	zassert_ok(k_sem_take(&ready, K_MSEC(500)));
	zassert_ok(k_sem_take(&ready, K_MSEC(500)));
	k_sem_give(&start);
	k_sem_give(&start);
	zassert_ok(k_thread_join(first_tid, K_MSEC(2000)));
	zassert_ok(k_thread_join(second_tid, K_MSEC(2000)));

	int hits = atomic_get(&hit_count);
	uint32_t value = g_riscv_amo32;
	bool active = k_watchpoint_is_active(&wp);

	zassert_ok(k_watchpoint_remove(&wp));
	zassert_ok(first.ret);
	zassert_ok(second.ret);
	zassert_equal(hits, first.iterations + second.iterations);
	zassert_equal(value, (uint32_t)(first.iterations + second.iterations));
	zassert_true(active);
}
#endif
#endif

struct smp_watchpoint_request {
	struct k_watchpoint *wp;
	bool add;
	int ret;
};

static void watchpoint_api_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct smp_watchpoint_request *request = p1;

	request->ret = request->add ? k_watchpoint_add(request->wp) :
				     k_watchpoint_remove(request->wp);
}

ZTEST(watchpoint, test_smp_add_remove_from_remote_cpu)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	unsigned int coordinator_cpu = arch_curr_cpu()->id;
	unsigned int remote_cpu =
		(coordinator_cpu + 1U) % arch_num_cpus();
	struct smp_watchpoint_request request = {
		.wp = &wp,
		.add = true,
		.ret = -EINPROGRESS,
	};

	reset_callback_state();
	zassert_ok(run_on_cpu(watchpoint_api_fn, remote_cpu, &request));
	if (request.ret == -ENOTSUP) {
		ztest_test_skip();
	}
	zassert_ok(request.ret);

	unsigned int key = irq_lock();
	unsigned int writer_cpu = arch_curr_cpu()->id;

	g_watched = 0x55AA55AAU;
	irq_unlock(key);
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_equal(g_cb_cpu, writer_cpu);

	request.add = false;
	request.ret = -EINPROGRESS;
	zassert_ok(run_on_cpu(watchpoint_api_fn, remote_cpu, &request));
	zassert_ok(request.ret);

	g_watched = 0xAA55AA55U;
	zassert_equal(atomic_get(&g_cb_count), 1);
}

struct concurrent_add_request {
	struct k_watchpoint *wp;
	struct k_sem *ready;
	struct k_sem *start;
	int ret;
};

static void concurrent_add_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct concurrent_add_request *request = p1;

	k_sem_give(request->ready);
	if (k_sem_take(request->start, K_MSEC(500)) != 0) {
		request->ret = -ETIMEDOUT;
		return;
	}

	request->ret = k_watchpoint_add(request->wp);
}

ZTEST(watchpoint, test_smp_concurrent_add_serialized)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	struct k_sem ready;
	struct k_sem start;
	struct concurrent_add_request first = {
		.wp = &wp,
		.ready = &ready,
		.start = &start,
		.ret = -EINPROGRESS,
	};
	struct concurrent_add_request second = {
		.wp = &wp,
		.ready = &ready,
		.start = &start,
		.ret = -EINPROGRESS,
	};
	unsigned int coordinator_cpu = arch_curr_cpu()->id;
	unsigned int remote_cpu =
		(coordinator_cpu + 1U) % arch_num_cpus();

	wp_add_or_skip(&wp);
	zassert_ok(k_watchpoint_remove(&wp));
	k_sem_init(&ready, 0, 2);
	k_sem_init(&start, 0, 2);

	k_tid_t first_tid = k_thread_create(
		&g_remover_thread, g_remover_stack, SMP_STACK_SIZE,
		concurrent_add_fn, &first, NULL, NULL,
		k_thread_priority_get(k_current_get()), 0, K_FOREVER);
	k_tid_t second_tid = k_thread_create(
		&g_adder_thread, g_adder_stack, SMP_STACK_SIZE,
		concurrent_add_fn, &second, NULL, NULL,
		k_thread_priority_get(k_current_get()), 0, K_FOREVER);

	k_thread_cpu_pin(first_tid, coordinator_cpu);
	k_thread_cpu_pin(second_tid, remote_cpu);
	k_thread_start(first_tid);
	k_thread_start(second_tid);
	zassert_ok(k_sem_take(&ready, K_MSEC(500)));
	zassert_ok(k_sem_take(&ready, K_MSEC(500)));
	k_sem_give(&start);
	k_sem_give(&start);
	zassert_ok(k_thread_join(first_tid, K_MSEC(2000)));
	zassert_ok(k_thread_join(second_tid, K_MSEC(2000)));

	zassert_true((first.ret == 0 && second.ret == -EBUSY) ||
		     (first.ret == -EBUSY && second.ret == 0),
		     "concurrent add returned %d and %d",
		     first.ret, second.ret);
	zassert_true(k_watchpoint_is_active(&wp));
	zassert_ok(k_watchpoint_remove(&wp));
}

#if defined(CONFIG_RISCV)
#define TEST_CSR_TSELECT 0x7a0
#define TEST_CSR_TDATA1  0x7a1
#define TEST_CSR_TDATA2  0x7a2

#define TEST_TDATA1_LOAD          BIT(0)
#define TEST_TDATA1_STORE         BIT(1)
#define TEST_TDATA1_EXECUTE       BIT(2)
#define TEST_TDATA1_M             BIT(6)
#define TEST_TDATA1_TIMING        BIT(18)
#define TEST_TDATA1_DMODE         BIT(__riscv_xlen - 5)
#define TEST_TDATA1_TYPE_SHIFT    (__riscv_xlen - 4)
#define TEST_TDATA1_TYPE_MASK     (0xfUL << TEST_TDATA1_TYPE_SHIFT)
#define TEST_TDATA1_MCONTROL      (2UL << TEST_TDATA1_TYPE_SHIFT)
#define TEST_TDATA1_MCONTROL6     (6UL << TEST_TDATA1_TYPE_SHIFT)

struct trigger_backup {
	uintptr_t tdata1;
	uintptr_t tdata2;
	int ret;
};

static struct trigger_backup g_trigger_backup;
static volatile uint32_t g_foreign_target __aligned(16);

static void reserve_foreign_trigger(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uintptr_t saved_tselect = csr_read_imm(TEST_CSR_TSELECT);

	csr_write_imm(TEST_CSR_TSELECT, 0);
	if (csr_read_imm(TEST_CSR_TSELECT) != 0U) {
		g_trigger_backup.ret = -ENOTSUP;
		csr_write_imm(TEST_CSR_TSELECT, saved_tselect);
		return;
	}

	uintptr_t tdata1 = csr_read_imm(TEST_CSR_TDATA1);
	uintptr_t type = tdata1 & TEST_TDATA1_TYPE_MASK;

	g_trigger_backup.tdata1 = tdata1;
	g_trigger_backup.tdata2 = csr_read_imm(TEST_CSR_TDATA2);
	if ((tdata1 & (TEST_TDATA1_DMODE | TEST_TDATA1_LOAD |
		       TEST_TDATA1_STORE | TEST_TDATA1_EXECUTE)) != 0U ||
	    (type != TEST_TDATA1_MCONTROL &&
	     type != TEST_TDATA1_MCONTROL6)) {
		g_trigger_backup.ret = -EBUSY;
		csr_write_imm(TEST_CSR_TSELECT, saved_tselect);
		return;
	}

	uintptr_t requested = type | TEST_TDATA1_M | TEST_TDATA1_STORE;

	if (type == TEST_TDATA1_MCONTROL) {
		requested |= TEST_TDATA1_TIMING;
	}

	csr_write_imm(TEST_CSR_TDATA1, 0);
	csr_write_imm(TEST_CSR_TDATA2, (uintptr_t)&g_foreign_target);
	csr_write_imm(TEST_CSR_TDATA1, requested);

	uintptr_t actual = csr_read_imm(TEST_CSR_TDATA1);

	if ((actual & (TEST_TDATA1_TYPE_MASK | TEST_TDATA1_STORE)) !=
		    (requested &
		     (TEST_TDATA1_TYPE_MASK | TEST_TDATA1_STORE)) ||
	    csr_read_imm(TEST_CSR_TDATA2) !=
		    (uintptr_t)&g_foreign_target) {
		csr_write_imm(TEST_CSR_TDATA1, 0);
		csr_write_imm(TEST_CSR_TDATA2, g_trigger_backup.tdata2);
		csr_write_imm(TEST_CSR_TDATA1, g_trigger_backup.tdata1);
		g_trigger_backup.ret = -ENOTSUP;
	} else {
		g_trigger_backup.ret = 0;
	}

	csr_write_imm(TEST_CSR_TSELECT, saved_tselect);
}

static void restore_foreign_trigger(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uintptr_t saved_tselect = csr_read_imm(TEST_CSR_TSELECT);

	csr_write_imm(TEST_CSR_TSELECT, 0);
	uintptr_t current = csr_read_imm(TEST_CSR_TDATA1);
	bool preserved =
		(current & (TEST_TDATA1_TYPE_MASK | TEST_TDATA1_STORE)) ==
		((g_trigger_backup.tdata1 & TEST_TDATA1_TYPE_MASK) |
		 TEST_TDATA1_STORE);
	preserved = preserved &&
		    csr_read_imm(TEST_CSR_TDATA2) ==
			    (uintptr_t)&g_foreign_target;

	csr_write_imm(TEST_CSR_TDATA1, 0);
	csr_write_imm(TEST_CSR_TDATA2, g_trigger_backup.tdata2);
	csr_write_imm(TEST_CSR_TDATA1, g_trigger_backup.tdata1);
	csr_write_imm(TEST_CSR_TSELECT, saved_tselect);
	g_trigger_backup.ret = preserved ? 0 : -EIO;
}

ZTEST(watchpoint, test_riscv_smp_remaps_around_foreign_trigger)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	static K_WATCHPOINT_DEFINE(flush, (void *)&g_unwatched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	static K_WATCHPOINT_DEFINE(wp, (void *)&g_watched, 1U,
				   K_WATCHPOINT_WRITE, callback, NULL);
	unsigned int remote_cpu =
		(arch_curr_cpu()->id + 1U) % arch_num_cpus();

	wp_add_or_skip(&flush);
	zassert_ok(k_watchpoint_remove(&flush));

	g_trigger_backup.ret = -EINPROGRESS;
	zassert_ok(run_on_cpu(reserve_foreign_trigger, remote_cpu, NULL));
	if (g_trigger_backup.ret != 0) {
		ztest_test_skip();
	}

	reset_callback_state();
	int add_ret = k_watchpoint_add(&wp);
	int writer_ret = 0;
	int remove_ret = 0;

	if (add_ret == 0) {
		writer_ret = run_on_cpu(writer_fn, remote_cpu,
					       (void *)&g_watched);
		remove_ret = k_watchpoint_remove(&wp);
	}

	int restore_ret =
		run_on_cpu(restore_foreign_trigger, remote_cpu, NULL);
	int preserve_ret = g_trigger_backup.ret;

	zassert_ok(restore_ret);
	zassert_ok(preserve_ret, "foreign trigger was modified");
	zassert_ok(add_ret);
	zassert_ok(writer_ret);
	zassert_ok(remove_ret);
	zassert_equal(atomic_get(&g_cb_count), 1);
	zassert_equal(g_cb_cpu, remote_cpu);
}
#endif
static atomic_t g_blocking_entered;
static atomic_t g_blocking_release;
static atomic_t g_remove_early;
static atomic_t g_remove_started;
static int g_remove_ret;
static int g_readd_ret;
static volatile uint32_t g_blocked __aligned(4);
static volatile uint32_t g_readd_target __aligned(4);
static K_WATCHPOINT_DEFINE(g_blocking_wp, (void *)&g_blocked, 1U,
			   K_WATCHPOINT_WRITE, NULL, NULL);

static void blocking_callback(const struct k_watchpoint *wp,
			      const struct k_watchpoint_event *event, void *arg)
{
	ARG_UNUSED(wp);
	ARG_UNUSED(event);
	ARG_UNUSED(arg);

	atomic_set(&g_blocking_entered, 1);
	while (atomic_get(&g_blocking_release) == 0) {
#if defined(CONFIG_ARM64)
		wfe();
#else
		compiler_barrier();
#endif
	}
}


static void remover_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	atomic_set(&g_remove_started, 1);
	g_remove_ret = k_watchpoint_remove((struct k_watchpoint *)p1);
	if (atomic_get(&g_blocking_release) == 0) {
		atomic_set(&g_remove_early, 1);
	}
}

static void readd_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct k_watchpoint *wp = p1;
	int64_t deadline = k_uptime_get() + 2000;

	while (atomic_get(&wp->_state) != 0 && k_uptime_get() < deadline) {
		k_yield();
	}
	if (atomic_get(&wp->_state) != 0) {
		g_readd_ret = -ETIMEDOUT;
		return;
	}

	wp->addr = (void *)&g_readd_target;
	g_readd_ret = k_watchpoint_add(wp);
}

ZTEST(watchpoint, test_smp_remove_waits_for_callback)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}

	g_blocking_wp.addr = (void *)&g_blocked;
	g_blocking_wp.cb = blocking_callback;
	atomic_set(&g_blocking_wp._state, 0);
	atomic_set(&g_blocking_entered, 0);
	atomic_set(&g_blocking_release, 0);
	atomic_set(&g_remove_early, 0);
	atomic_set(&g_remove_started, 0);
	g_remove_ret = -EINPROGRESS;
	g_readd_ret = -EINPROGRESS;
	wp_add_or_skip(&g_blocking_wp);

	unsigned int coordinator_cpu = arch_curr_cpu()->id;
	unsigned int writer_cpu =
		(coordinator_cpu + 1U) % arch_num_cpus();
	k_tid_t writer = k_thread_create(
		&g_writer_thread, g_writer_stack, SMP_STACK_SIZE, writer_fn,
		(void *)&g_blocked, NULL, NULL,
		k_thread_priority_get(k_current_get()), 0, K_FOREVER);

	k_thread_cpu_pin(writer, writer_cpu);
	k_thread_start(writer);

	int64_t deadline = k_uptime_get() + 500;

	while (atomic_get(&g_blocking_entered) == 0 &&
	       k_uptime_get() < deadline) {
		k_yield();
	}
	zassert_equal(atomic_get(&g_blocking_entered), 1);

	k_tid_t remover = k_thread_create(
		&g_remover_thread, g_remover_stack, SMP_STACK_SIZE, remover_fn,
		&g_blocking_wp, NULL, NULL,
		k_thread_priority_get(k_current_get()), 0, K_FOREVER);

	k_thread_cpu_pin(remover, coordinator_cpu);
	k_thread_start(remover);
	while (atomic_get(&g_remove_started) == 0) {
		k_yield();
	}
	zassert_equal(g_remove_ret, -EINPROGRESS);

	k_tid_t adder = k_thread_create(
		&g_adder_thread, g_adder_stack, SMP_STACK_SIZE, readd_fn,
		&g_blocking_wp, NULL, NULL,
		k_thread_priority_get(k_current_get()), 0, K_FOREVER);

	k_thread_cpu_pin(adder, coordinator_cpu);
	k_thread_start(adder);
	atomic_set(&g_blocking_release, 1);
#if defined(CONFIG_ARM64)
	sev();
#endif

	zassert_ok(k_thread_join(&g_remover_thread, K_MSEC(2000)));
	zassert_ok(k_thread_join(&g_writer_thread, K_MSEC(2000)));
	zassert_ok(k_thread_join(&g_adder_thread, K_MSEC(2000)));
	zassert_ok(g_remove_ret);
	zassert_equal(atomic_get(&g_remove_early), 0);
	zassert_ok(g_readd_ret);
	zassert_true(k_watchpoint_is_active(&g_blocking_wp));
	zassert_ok(k_watchpoint_remove(&g_blocking_wp));
}
#endif
