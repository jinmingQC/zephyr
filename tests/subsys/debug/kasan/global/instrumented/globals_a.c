/* SPDX-License-Identifier: Apache-2.0 */

#include "global_test.h"

#include <zephyr/toolchain.h>

#include <string.h>

static volatile uint8_t global_a[16];

uintptr_t global_a_address(void)
{
	return (uintptr_t)global_a;
}

uint8_t global_a_read(size_t offset)
{
	return global_a[offset];
}

uint64_t global_a_read8(size_t offset)
{
	typedef uint64_t __aligned(1) unaligned_u64_t;

	return *(const volatile unaligned_u64_t *)&global_a[offset];
}

void global_a_write(size_t offset, uint8_t value)
{
	global_a[offset] = value;
}

void global_a_write8(size_t offset, uint64_t value)
{
	typedef uint64_t __aligned(1) unaligned_u64_t;

	*(volatile unaligned_u64_t *)&global_a[offset] = value;
}

void global_a_copy_to(void *destination, size_t size)
{
	memcpy(destination, (const void *)global_a, size);
}

void global_a_fill(size_t offset, size_t size)
{
	memset((void *)&global_a[offset], 0xa5, size);
}
