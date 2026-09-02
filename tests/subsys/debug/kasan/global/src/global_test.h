/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ZEPHYR_TESTS_SUBSYS_DEBUG_KASAN_GLOBAL_TEST_H_
#define ZEPHYR_TESTS_SUBSYS_DEBUG_KASAN_GLOBAL_TEST_H_

#include <stddef.h>
#include <stdint.h>

uintptr_t global_a_address(void);
uint8_t global_a_read(size_t offset);
uint64_t global_a_read8(size_t offset);
void global_a_write(size_t offset, uint8_t value);
void global_a_write8(size_t offset, uint64_t value);
void global_a_copy_to(void *destination, size_t size);
void global_a_fill(size_t offset, size_t size);

uintptr_t global_b_address(void);
uint8_t global_b_read(size_t offset);
void global_b_write(size_t offset, uint8_t value);

uintptr_t function_static_address(void);
uint8_t function_static_read(size_t offset);
void function_static_write(size_t offset, uint8_t value);

#endif /* ZEPHYR_TESTS_SUBSYS_DEBUG_KASAN_GLOBAL_TEST_H_ */
