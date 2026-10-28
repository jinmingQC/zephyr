/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CPP_USER_TEST_HELPERS_H_
#define CPP_USER_TEST_HELPERS_H_

#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test instrumentation only: ARM's raw cycle accessor is privileged. */
__syscall uint32_t cpp_test_cycle_get32(void);

#ifdef __cplusplus
}
#endif

#include <zephyr/syscalls/user_test_helpers.h>
#endif
