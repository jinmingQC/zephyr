/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LIB_CPP_LIBCXX_STDIO_H_
#define ZEPHYR_LIB_CPP_LIBCXX_STDIO_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a real picolibc stream over native Zephyr filesystem operations. */
FILE *z_libcxx_fopen(const char *name, const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_LIB_CPP_LIBCXX_STDIO_H_ */
