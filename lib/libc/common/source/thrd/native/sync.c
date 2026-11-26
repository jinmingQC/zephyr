/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <threads.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/timeutil.h>

static struct k_spinlock init_lock;

static int mutex_init(mtx_t *mutex, uint32_t options)
{
	int ret = k_mutex_init_ext(&mutex->kernel, options);

	if (ret == 0) {
		atomic_set(&mutex->initialized, 1);
	}

	return ret;
}

/* libc++ initializes its static mutexes with {}. Initialize the wait queue
 * before the object is used the first time.
 */
static void ensure_mutex_initialized(mtx_t *mutex)
{
	if (atomic_get(&mutex->initialized) == 0) {
		K_SPINLOCK(&init_lock) {
			if (atomic_get(&mutex->initialized) == 0) {
				(void)mutex_init(mutex, K_MUTEX_NORMAL);
			}
		}
	}
}

int mtx_init(mtx_t *mutex, int type)
{
	uint32_t options;

	switch (type) {
	case mtx_plain:
	case mtx_timed:
		options = K_MUTEX_NORMAL;
		break;
	case mtx_recursive:
	case mtx_timed | mtx_recursive:
		options = K_MUTEX_RECURSIVE;
		break;
	default:
		return thrd_error;
	}

	return mutex_init(mutex, options) == 0 ? thrd_success : thrd_error;
}

void mtx_destroy(mtx_t *mutex)
{
#ifdef CONFIG_OBJ_CORE_MUTEX
	if (atomic_get(&mutex->initialized) != 0) {
		k_obj_core_unlink(K_OBJ_CORE(&mutex->kernel));
	}
#else
	ARG_UNUSED(mutex);
#endif
}

int mtx_lock(mtx_t *mutex)
{
	ensure_mutex_initialized(mutex);
	return k_mutex_lock(&mutex->kernel, K_FOREVER) == 0 ? thrd_success : thrd_error;
}

int mtx_timedlock(mtx_t *restrict mutex, const struct timespec *restrict deadline)
{
	k_timeout_t timeout = sys_timepoint_timeout(timespec_abs_rt_to_timepoint(deadline));

	ensure_mutex_initialized(mutex);
	switch (k_mutex_lock(&mutex->kernel, timeout)) {
	case 0:
		return thrd_success;
	case -EBUSY:
	case -EAGAIN:
		return thrd_timedout;
	default:
		return thrd_error;
	}
}

int mtx_trylock(mtx_t *mutex)
{
	ensure_mutex_initialized(mutex);
	switch (k_mutex_lock(&mutex->kernel, K_NO_WAIT)) {
	case 0:
		return thrd_success;
	case -EBUSY:
		return thrd_busy;
	default:
		return thrd_error;
	}
}

int mtx_unlock(mtx_t *mutex)
{
	return k_mutex_unlock(&mutex->kernel) == 0 ? thrd_success : thrd_error;
}

int cnd_init(cnd_t *cond)
{
	int ret = k_condvar_init(&cond->kernel);

	if (ret != 0) {
		return thrd_error;
	}

	atomic_set(&cond->initialized, 1);
	return thrd_success;
}

static void ensure_condvar_initialized(cnd_t *cond)
{
	if (atomic_get(&cond->initialized) == 0) {
		K_SPINLOCK(&init_lock) {
			if (atomic_get(&cond->initialized) == 0) {
				(void)cnd_init(cond);
			}
		}
	}
}

void cnd_destroy(cnd_t *cond)
{
#ifdef CONFIG_OBJ_CORE_CONDVAR
	if (atomic_get(&cond->initialized) != 0) {
		k_obj_core_unlink(K_OBJ_CORE(&cond->kernel));
	}
#else
	ARG_UNUSED(cond);
#endif
}

int cnd_signal(cnd_t *cond)
{
	ensure_condvar_initialized(cond);
	return k_condvar_signal(&cond->kernel) >= 0 ? thrd_success : thrd_error;
}

int cnd_broadcast(cnd_t *cond)
{
	ensure_condvar_initialized(cond);
	return k_condvar_broadcast(&cond->kernel) >= 0 ? thrd_success : thrd_error;
}

int cnd_wait(cnd_t *cond, mtx_t *mutex)
{
	ensure_condvar_initialized(cond);
	return k_condvar_wait(&cond->kernel, &mutex->kernel, K_FOREVER) == 0 ?
		thrd_success : thrd_error;
}

int cnd_timedwait(cnd_t *restrict cond, mtx_t *restrict mutex,
		  const struct timespec *restrict deadline)
{
	k_timeout_t timeout = sys_timepoint_timeout(timespec_abs_rt_to_timepoint(deadline));

	ensure_condvar_initialized(cond);
	switch (k_condvar_wait(&cond->kernel, &mutex->kernel, timeout)) {
	case 0:
		return thrd_success;
	case -EAGAIN:
		return thrd_timedout;
	default:
		return thrd_error;
	}
}

void call_once(once_flag *flag, void (*func)(void))
{
	sys_thread_once(flag, func);
}
