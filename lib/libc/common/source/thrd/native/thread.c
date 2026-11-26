/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <threads.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/thread.h>

static void thrd_trampoline(void *p1, void *p2, void *p3)
{
	thrd_start_t func = (thrd_start_t)p1;

	ARG_UNUSED(p3);

	k_thread_exit(INT_TO_POINTER(func(p2)));
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
	int priority = k_thread_priority_get(k_current_get());

	switch (sys_thread_create(thr, NULL, 0, 0, thrd_trampoline, (void *)func, arg, NULL,
				  priority, 0)) {
	case 0:
		return thrd_success;
	case -ENOMEM:
		return thrd_nomem;
	default:
		return thrd_error;
	}
}

int thrd_equal(thrd_t lhs, thrd_t rhs)
{
	return lhs == rhs;
}

thrd_t thrd_current(void)
{
	return k_current_get();
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
	int ret = sys_clock_nanosleep(SYS_CLOCK_MONOTONIC, 0, duration, remaining);

	if (ret == -EINTR) {
		return -1;
	}

	return ret == 0 ? 0 : -2;
}

void thrd_yield(void)
{
	k_yield();
}

FUNC_NORETURN void thrd_exit(int res)
{
	k_thread_exit(INT_TO_POINTER(res));

	CODE_UNREACHABLE;
}

int thrd_detach(thrd_t thr)
{
	switch (k_thread_detach(thr)) {
	case 0:
		break;
	default:
		return thrd_error;
	}

	return thrd_success;
}

int thrd_join(thrd_t thr, int *res)
{
	void *ret = NULL;

	switch (k_thread_rejoin(thr, &ret, K_FOREVER)) {
	case 0:
		if (res != NULL) {
			*res = POINTER_TO_INT(ret);
		}
		return thrd_success;
	default:
		return thrd_error;
	}
}

int tss_create(tss_t *key, tss_dtor_t destructor)
{
	switch (k_thread_key_create(key, destructor)) {
	case 0:
		return thrd_success;
	case -ENOMEM:
		return thrd_nomem;
	default:
		return thrd_error;
	}
}

void *tss_get(tss_t key)
{
	void *value = NULL;

	if (k_thread_getspecific(key, &value) == 0) {
		return value;
	}

	return NULL;
}

int tss_set(tss_t key, void *val)
{
	switch (k_thread_setspecific(key, val)) {
	case 0:
		return thrd_success;
	case -ENOMEM:
		return thrd_nomem;
	default:
		return thrd_error;
	}
}

void tss_delete(tss_t key)
{
	(void)k_thread_key_delete(key);
}
