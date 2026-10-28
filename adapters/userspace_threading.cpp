/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <__external_threading>
#include <cpp_runtime/native_syscalls.h>
#include <cpp_runtime/thread.h>
#include <cpp_runtime/partition.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" {
unsigned char cpp_runtime_data_anchor = 1;
unsigned char cpp_runtime_bss_anchor;
}

namespace {

/* Some libc++ internal users intentionally ignore error returns. Never let
 * resource exhaustion turn a lock into an unprotected critical section. */
int require_success(int result)
{
	if (result != 0) {
		printk("libc++ synchronization failure: %d\n", result);
		abort();
	}
	return 0;
}

std::__libcpp_mutex_t initialization_lock{};
std::__libcpp_mutex_t once_lock{};
std::__libcpp_condvar_t once_cond{};

using tls_destructor = void (*)(void *);
tls_destructor tls_destructors[CONFIG_CPP_RUNTIME_TLS_KEYS];
atomic_t tls_key_count;
thread_local void *tls_values[CONFIG_CPP_RUNTIME_TLS_KEYS];

struct thread_destructor {
	tls_destructor destructor;
	void *object;
	thread_destructor *next;
};

thread_local thread_destructor *thread_destructors;

void run_thread_destructors()
{
	/* C++ thread_local destructors precede notify_all_at_thread_exit(). */
	while (thread_destructors != nullptr) {
		thread_destructor *node = thread_destructors;

		thread_destructors = node->next;
		node->destructor(node->object);
		free(node);
	}

	/* As with POSIX TSS, a destructor may populate another key. */
	for (uint32_t pass = 0; pass < 4U; ++pass) {
		bool called = false;
		uint32_t count = static_cast<uint32_t>(atomic_get(&tls_key_count));

		for (uint32_t key = 0; key < count; ++key) {
			void *value = tls_values[key];

			if (value != nullptr && tls_destructors[key] != nullptr) {
				tls_values[key] = nullptr;
				tls_destructors[key](value);
				called = true;
			}
		}
		if (!called) {
			break;
		}
	}
}

int wait_condvar(std::__libcpp_condvar_t *cond, std::__libcpp_mutex_t *mutex,
                 k_timeout_t timeout)
{
	int ret = cpp_runtime_sync(&cond->handle, CPP_RUNTIME_COND_WAIT,
				   &mutex->handle, timeout.ticks);

	return ret == -EAGAIN ? ETIMEDOUT : require_success(ret);
}

} /* namespace */

extern "C" void cpp_runtime_thread_cleanup(void)
{
	run_thread_destructors();
}

/* The kernel chooses K_USER before entering this trampoline. Application
 * callbacks and destructors are never executed by a syscall handler. */
extern "C" void cpp_runtime_thread_entry(void *entry, void *arg, void *)
{
	auto function = reinterpret_cast<void *(*)(void *)>(entry);

	(void)function(arg);
	cpp_runtime_thread_cleanup();
}

extern "C" int __cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *dso)
{
	ARG_UNUSED(dso);

	auto *node = static_cast<thread_destructor *>(malloc(sizeof(thread_destructor)));

	if (node == nullptr) {
		/* Compilers do not check registration failure for thread_local. */
		printk("libc++ TLS destructor registration failure: %d\n", -ENOMEM);
		abort();
	}
	node->destructor = destructor;
	node->object = object;
	node->next = thread_destructors;
	thread_destructors = node;
	return 0;
}

_LIBCPP_BEGIN_NAMESPACE_STD

int __libcpp_recursive_mutex_init(__libcpp_recursive_mutex_t *mutex)
{
	mutex->handle = 0;
	return require_success(cpp_runtime_sync(&mutex->handle, CPP_RUNTIME_MUTEX_INIT, nullptr, 0));
}

int __libcpp_recursive_mutex_lock(__libcpp_recursive_mutex_t *mutex)
{
	return __libcpp_mutex_lock(mutex);
}

bool __libcpp_recursive_mutex_trylock(__libcpp_recursive_mutex_t *mutex)
{
	return __libcpp_mutex_trylock(mutex);
}

int __libcpp_recursive_mutex_unlock(__libcpp_recursive_mutex_t *mutex)
{
	return __libcpp_mutex_unlock(mutex);
}

int __libcpp_recursive_mutex_destroy(__libcpp_recursive_mutex_t *mutex)
{
	return __libcpp_mutex_destroy(mutex);
}

int __libcpp_mutex_lock(__libcpp_mutex_t *mutex)
{
	return require_success(cpp_runtime_sync(&mutex->handle, CPP_RUNTIME_MUTEX_LOCK, nullptr, -1));
}

bool __libcpp_mutex_trylock(__libcpp_mutex_t *mutex)
{
	int ret = cpp_runtime_sync(&mutex->handle, CPP_RUNTIME_MUTEX_TRYLOCK, nullptr, 0);

	if (ret == -EBUSY) {
		return false;
	}
	return require_success(ret) == 0;
}

int __libcpp_mutex_unlock(__libcpp_mutex_t *mutex)
{
	return require_success(cpp_runtime_sync(&mutex->handle, CPP_RUNTIME_MUTEX_UNLOCK, nullptr, 0));
}

int __libcpp_mutex_destroy(__libcpp_mutex_t *mutex)
{
	return require_success(cpp_runtime_sync(&mutex->handle, CPP_RUNTIME_MUTEX_DESTROY, nullptr, 0));
}

int __libcpp_condvar_signal(__libcpp_condvar_t *cond)
{
	int ret = cpp_runtime_sync(&cond->handle, CPP_RUNTIME_COND_SIGNAL, nullptr, 0);

	return require_success(ret);
}

int __libcpp_condvar_broadcast(__libcpp_condvar_t *cond)
{
	int ret = cpp_runtime_sync(&cond->handle, CPP_RUNTIME_COND_BROADCAST, nullptr, 0);

	return require_success(ret);
}

int __libcpp_condvar_wait(__libcpp_condvar_t *cond, __libcpp_mutex_t *mutex)
{
	return wait_condvar(cond, mutex, K_FOREVER);
}

int __libcpp_condvar_timedwait(__libcpp_condvar_t *cond, __libcpp_mutex_t *mutex,
			     __libcpp_timespec_t *deadline)
{
	if (deadline->tv_nsec < 0 || static_cast<uint64_t>(deadline->tv_nsec) >= NSEC_PER_SEC) {
		return EINVAL;
	}

	/*
	 * Bound each native wait to one day. This avoids time conversion
	 * overflow, while rechecking CLOCK_REALTIME for longer deadlines.
	 */
	for (;;) {
		struct timespec now;

		if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
			return errno;
		}
		if (deadline->tv_sec < now.tv_sec ||
		    (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec)) {
			return wait_condvar(cond, mutex, K_NO_WAIT);
		}
		uint64_t seconds = static_cast<uint64_t>(deadline->tv_sec) -
				   static_cast<uint64_t>(now.tv_sec);
		int64_t nanos = deadline->tv_nsec - now.tv_nsec;

		if (nanos < 0) {
			--seconds;
			nanos += NSEC_PER_SEC;
		}
		bool chunked = seconds >= 86400U;
		k_timeout_t timeout = chunked ? K_SECONDS(86400) :
			K_NSEC(seconds * NSEC_PER_SEC + static_cast<uint64_t>(nanos));
		int ret = wait_condvar(cond, mutex, timeout);

		if (ret != ETIMEDOUT || !chunked) {
			return ret;
		}
	}
}

int __libcpp_condvar_destroy(__libcpp_condvar_t *cond)
{
	return require_success(cpp_runtime_sync(&cond->handle, CPP_RUNTIME_COND_DESTROY, nullptr, 0));
}

int __libcpp_execute_once(__libcpp_exec_once_flag *flag, void (*init_routine)())
{
	if (atomic_get(flag) == 2) {
		return 0;
	}
	int ret = -__libcpp_mutex_lock(&once_lock);

	if (ret != 0) {
		return -ret;
	}
	while (atomic_get(flag) == 1) {
		ret = -__libcpp_condvar_wait(&once_cond, &once_lock);
		if (ret != 0) {
			(void)-__libcpp_mutex_unlock(&once_lock);
			return -ret;
		}
	}
	if (atomic_get(flag) == 2) {
		return __libcpp_mutex_unlock(&once_lock);
	}
	atomic_set(flag, 1);
	ret = -__libcpp_mutex_unlock(&once_lock);
	if (ret != 0) {
		return -ret;
	}
#ifndef _LIBCPP_HAS_NO_EXCEPTIONS
	try {
#endif
		init_routine();
#ifndef _LIBCPP_HAS_NO_EXCEPTIONS
	} catch (...) {
		ret = -__libcpp_mutex_lock(&once_lock);
		__ASSERT_NO_MSG(ret == 0);
		atomic_set(flag, 0);
		ret = -__libcpp_mutex_unlock(&once_lock);
		__ASSERT_NO_MSG(ret == 0);
		(void)-__libcpp_condvar_broadcast(&once_cond);
		throw;
	}
#endif
	ret = -__libcpp_mutex_lock(&once_lock);
	if (ret != 0) {
		return -ret;
	}
	atomic_set(flag, 2);
	ret = -__libcpp_mutex_unlock(&once_lock);
	if (ret != 0) {
		return -ret;
	}
	ret = -__libcpp_condvar_broadcast(&once_cond);
	return require_success(ret);
}

bool __libcpp_thread_id_equal(__libcpp_thread_id first, __libcpp_thread_id second)
{
	return first == second;
}

bool __libcpp_thread_id_less(__libcpp_thread_id first, __libcpp_thread_id second)
{
	return reinterpret_cast<uintptr_t>(first) < reinterpret_cast<uintptr_t>(second);
}

bool __libcpp_thread_isnull(const __libcpp_thread_t *thread)
{
	return *thread == nullptr;
}

int __libcpp_thread_create(__libcpp_thread_t *thread, void *(*entry)(void *), void *arg)
{
	return -cpp_runtime_thread_create(thread, entry, arg);
}

__libcpp_thread_id __libcpp_thread_get_current_id()
{
	return k_current_get();
}

__libcpp_thread_id __libcpp_thread_get_id(const __libcpp_thread_t *thread)
{
	return *thread;
}

int __libcpp_thread_join(__libcpp_thread_t *thread)
{
	return -cpp_runtime_thread_join(*thread);
}

int __libcpp_thread_detach(__libcpp_thread_t *thread)
{
	return -cpp_runtime_thread_detach(*thread);
}

void __libcpp_thread_yield()
{
	k_yield();
}

void __libcpp_thread_sleep_for(const chrono::nanoseconds &duration)
{
	if (duration.count() <= 0) {
		return;
	}
	/* sys_timepoint_* directly access scheduler state and are supervisor-only.
	 * Use the checked uptime syscall, with bounded waits and elapsed arithmetic
	 * so k_wakeup cannot shorten the requested sleep. The initial extra tick
	 * compensates for reading a truncated tick count near a tick boundary. */
	const uint64_t start = static_cast<uint64_t>(k_uptime_ticks());
	const uint64_t ticks = k_ns_to_ticks_ceil64(duration.count()) + 1U;
	const uint64_t chunk = k_sec_to_ticks_ceil64(86400);

	for (;;) {
		uint64_t elapsed = static_cast<uint64_t>(k_uptime_ticks()) - start;

		if (elapsed >= ticks) {
			break;
		}
		uint64_t remaining = ticks - elapsed;

		(void)k_sleep(K_TICKS(static_cast<k_ticks_t>(remaining < chunk ? remaining : chunk)));
	}
}

int __libcpp_tls_create(__libcpp_tls_key *key, void (*destructor)(void *))
{
	int ret = -__libcpp_mutex_lock(&initialization_lock);

	if (ret != 0) {
		return -ret;
	}
	uint32_t count = static_cast<uint32_t>(atomic_get(&tls_key_count));

	if (count >= CONFIG_CPP_RUNTIME_TLS_KEYS) {
		(void)-__libcpp_mutex_unlock(&initialization_lock);
		return EAGAIN;
	}
	tls_destructors[count] = destructor;
	atomic_set(&tls_key_count, count + 1U);
	*key = count;
	return __libcpp_mutex_unlock(&initialization_lock);
}

void *__libcpp_tls_get(__libcpp_tls_key key)
{
	if (key >= static_cast<uint32_t>(atomic_get(&tls_key_count))) {
		return nullptr;
	}
	return tls_values[key];
}

int __libcpp_tls_set(__libcpp_tls_key key, void *value)
{
	if (key >= static_cast<uint32_t>(atomic_get(&tls_key_count))) {
		return EINVAL;
	}
	tls_values[key] = value;
	return 0;
}

_LIBCPP_END_NAMESPACE_STD
