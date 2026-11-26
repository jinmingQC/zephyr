/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LIB_LIBC_COMMON_INCLUDE_MACHINE__THREADS_H_
#define ZEPHYR_LIB_LIBC_COMMON_INCLUDE_MACHINE__THREADS_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_COMMON_LIBC_THRD_NATIVE
#include <zephyr/kernel.h>
#include <zephyr/sys/thread_once.h>
#endif

#define ONCE_FLAG_INIT {0}

#ifdef CONFIG_COMMON_LIBC_THRD_NATIVE
typedef struct {
	struct k_condvar kernel;
	atomic_t initialized;
} cnd_t;

typedef struct {
	struct k_mutex kernel;
	atomic_t initialized;
} mtx_t;

typedef k_tid_t thrd_t;
typedef void *tss_t;
typedef sys_thread_once_t once_flag;
#else
typedef int cnd_t;
typedef int mtx_t;
typedef int thrd_t;
typedef int tss_t;
typedef struct {
	char flag;
} once_flag;
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_LIB_LIBC_COMMON_INCLUDE_MACHINE__THREADS_H_ */
