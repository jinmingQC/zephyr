/* SPDX-License-Identifier: Apache-2.0 */

#include "global_test.h"

static volatile uint8_t global_b[31];

static volatile uint8_t *function_static_array(void)
{
	static volatile uint8_t function_static[19];

	return function_static;
}

uintptr_t global_b_address(void)
{
	return (uintptr_t)global_b;
}

uint8_t global_b_read(size_t offset)
{
	return global_b[offset];
}

void global_b_write(size_t offset, uint8_t value)
{
	global_b[offset] = value;
}

uintptr_t function_static_address(void)
{
	return (uintptr_t)function_static_array();
}

uint8_t function_static_read(size_t offset)
{
	return function_static_array()[offset];
}

void function_static_write(size_t offset, uint8_t value)
{
	function_static_array()[offset] = value;
}
