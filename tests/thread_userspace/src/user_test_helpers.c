// SPDX-License-Identifier: Apache-2.0

#include <user_test_helpers.h>
#include <zephyr/internal/syscall_handler.h>

uint32_t z_impl_cpp_test_cycle_get32(void)
{
	return k_cycle_get_32();
}

static inline uint32_t z_vrfy_cpp_test_cycle_get32(void)
{
	return z_impl_cpp_test_cycle_get32();
}
#include <zephyr/syscalls/cpp_test_cycle_get32_mrsh.c>
