/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/debug/kasan.h>
#include <zephyr/ztest.h>

#include <setjmp.h>
#include <string.h>

#include "global_test.h"

static jmp_buf violation_jump;
static bool violation_expected;
static struct kasan_fault last_fault;

void kasan_report(const struct kasan_fault *fault)
{
	if (!violation_expected) {
		TC_PRINT("unexpected KASAN fault at %p\n", (void *)fault->address);
		ztest_test_fail();
		return;
	}

	last_fault = *fault;
	violation_expected = false;
	longjmp(violation_jump, 1);
}

static void expect_fault(enum kasan_access_type access, const char *object_name,
			 uintptr_t object_address, size_t object_size, void (*operation)(void))
{
	violation_expected = true;
	if (setjmp(violation_jump) == 0) {
		operation();
		violation_expected = false;
		ztest_test_fail();
		return;
	}

	zassert_equal(last_fault.access, access);
	zassert_equal(last_fault.memory, KASAN_MEMORY_GLOBAL);
	zassert_equal(last_fault.region_start, object_address);
	zassert_equal(last_fault.object_size, object_size);
	zassert_equal(last_fault.first_bad_address, object_address + object_size);
	zassert_not_equal(last_fault.pc, 0U);
	zassert_not_null(last_fault.object_name);
	zassert_not_null(strstr(last_fault.object_name, object_name));

#if !defined(__clang__)
	/* GCC emits source metadata for every descriptor covered by this test. */
	zassert_not_null(last_fault.source_file);
#endif
	if (last_fault.source_file != NULL) {
		zassert_not_equal(last_fault.source_line, 0U);
	}
}

static void write_a_oob(void)
{
	global_a_write(16, 0xff);
}

static void read_a_oob(void)
{
	(void)global_a_read(16);
}

static void read_a_cross_boundary(void)
{
	(void)global_a_read8(12);
}

static void write_a_cross_boundary(void)
{
	global_a_write8(12, UINT64_C(0x0123456789abcdef));
}

static void write_b_oob(void)
{
	global_b_write(31, 0xff);
}

static void read_function_static_oob(void)
{
	(void)function_static_read(19);
}

static void copy_a_oob(void)
{
	uint8_t destination[32];

	global_a_copy_to(destination, 17);
}

static void fill_a_oob(void)
{
	global_a_fill(0, 17);
}

ZTEST(kasan_global, test_in_bounds_accesses)
{
	global_a_write(0, 0x12);
	global_a_write(15, 0x34);
	zassert_equal(global_a_read(0), 0x12);
	zassert_equal(global_a_read(15), 0x34);

	global_b_write(0, 0x56);
	global_b_write(30, 0x78);
	zassert_equal(global_b_read(0), 0x56);
	zassert_equal(global_b_read(30), 0x78);
}

ZTEST(kasan_global, test_write_overflow)
{
	expect_fault(KASAN_ACCESS_WRITE, "global_a", global_a_address(), 16, write_a_oob);
}

ZTEST(kasan_global, test_read_overflow)
{
	expect_fault(KASAN_ACCESS_READ, "global_a", global_a_address(), 16, read_a_oob);
}

ZTEST(kasan_global, test_read_crosses_boundary)
{
	expect_fault(KASAN_ACCESS_READ, "global_a", global_a_address(), 16,
		     read_a_cross_boundary);
}

ZTEST(kasan_global, test_write_crosses_boundary)
{
	expect_fault(KASAN_ACCESS_WRITE, "global_a", global_a_address(), 16,
		     write_a_cross_boundary);
}

ZTEST(kasan_global, test_second_translation_unit)
{
	expect_fault(KASAN_ACCESS_WRITE, "global_b", global_b_address(), 31, write_b_oob);
}

ZTEST(kasan_global, test_function_static)
{
	function_static_write(18, 0x5a);
	zassert_equal(function_static_read(18), 0x5a);
	expect_fault(KASAN_ACCESS_READ, "function_static", function_static_address(), 19,
		     read_function_static_oob);
}

ZTEST(kasan_global, test_memcpy_source_read)
{
	expect_fault(KASAN_ACCESS_READ, "global_a", global_a_address(), 16, copy_a_oob);
}

ZTEST(kasan_global, test_memset_destination_write)
{
	expect_fault(KASAN_ACCESS_WRITE, "global_a", global_a_address(), 16, fill_a_oob);
}

ZTEST_SUITE(kasan_global, NULL, NULL, NULL, NULL, NULL);
