/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <__external_threading>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

namespace {

K_MUTEX_DEFINE(initialization_lock);
K_MUTEX_DEFINE(thread_pool_lock);
K_MUTEX_DEFINE(once_lock);
K_CONDVAR_DEFINE(once_cond);

struct thread_slot {
	struct k_thread native;
	void *(*entry)(void *);
	void *arg;
	bool used;
	bool detached;
	bool finished;
	bool joining;
};

thread_slot thread_slots[CONFIG_LIBCXX_THREAD_MAX];
K_THREAD_STACK_ARRAY_DEFINE(thread_stacks, CONFIG_LIBCXX_THREAD_MAX,
			   CONFIG_LIBCXX_THREAD_STACK_SIZE);

using tls_destructor = void (*)(void *);
tls_destructor tls_destructors[CONFIG_LIBCXX_TLS_KEYS];
atomic_t tls_key_count;
thread_local void *tls_values[CONFIG_LIBCXX_TLS_KEYS];

struct thread_destructor {
	tls_destructor destructor;
	void *object;
	thread_destructor *next;
};

thread_local thread_destructor *thread_destructors;

int ensure_mutex(std::__libcpp_mutex_t *mutex)
{
	if (atomic_get(&mutex->initialized) != 0) {
		return 0;
	}

	int ret = k_mutex_lock(&initialization_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	if (atomic_get(&mutex->initialized) == 0) {
		ret = k_mutex_init(&mutex->native);
		if (ret == 0) {
			atomic_set(&mutex->initialized, 1);
		}
	}
	int unlock_ret = k_mutex_unlock(&initialization_lock);

	return ret != 0 ? -ret : -unlock_ret;
}

int ensure_condvar(std::__libcpp_condvar_t *cond)
{
	if (atomic_get(&cond->initialized) != 0) {
		return 0;
	}

	int ret = k_mutex_lock(&initialization_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	if (atomic_get(&cond->initialized) == 0) {
		ret = k_condvar_init(&cond->native);
		if (ret == 0) {
			atomic_set(&cond->initialized, 1);
		}
	}
	int unlock_ret = k_mutex_unlock(&initialization_lock);

	return ret != 0 ? -ret : -unlock_ret;
}

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

void thread_entry(void *context, void *, void *)
{
	auto *slot = static_cast<thread_slot *>(context);

	(void)slot->entry(slot->arg);
	run_thread_destructors();

	int ret = k_mutex_lock(&thread_pool_lock, K_FOREVER);

	__ASSERT_NO_MSG(ret == 0);
	slot->finished = true;
	ret = k_mutex_unlock(&thread_pool_lock);
	__ASSERT_NO_MSG(ret == 0);
	/* The slot cannot be reused until k_thread_join confirms termination. */
}

thread_slot *find_thread(k_tid_t thread)
{
	for (auto &slot : thread_slots) {
		if (&slot.native == thread && slot.used) {
			return &slot;
		}
	}
	return nullptr;
}

int wait_condvar(std::__libcpp_condvar_t *cond, std::__libcpp_mutex_t *mutex,
		 k_timeout_t timeout)
{
	int ret = ensure_condvar(cond);

	if (ret == 0) {
		ret = ensure_mutex(mutex);
	}
	if (ret != 0) {
		return ret;
	}
	ret = k_condvar_wait(&cond->native, &mutex->native, timeout);
	return ret == -EAGAIN ? ETIMEDOUT : -ret;
}

} /* namespace */

extern "C" int __cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *dso)
{
	ARG_UNUSED(dso);

	auto *node = static_cast<thread_destructor *>(malloc(sizeof(thread_destructor)));

	if (node == nullptr) {
		return -1;
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
	/* Unlike std::mutex, recursive_mutex has no constexpr initializer. */
	atomic_set(&mutex->initialized, 0);
	return ensure_mutex(mutex);
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
	int ret = ensure_mutex(mutex);

	return ret != 0 ? ret : -k_mutex_lock(&mutex->native, K_FOREVER);
}

bool __libcpp_mutex_trylock(__libcpp_mutex_t *mutex)
{
	return ensure_mutex(mutex) == 0 && k_mutex_lock(&mutex->native, K_NO_WAIT) == 0;
}

int __libcpp_mutex_unlock(__libcpp_mutex_t *mutex)
{
	if (atomic_get(&mutex->initialized) == 0) {
		return EINVAL;
	}
	return -k_mutex_unlock(&mutex->native);
}

int __libcpp_mutex_destroy(__libcpp_mutex_t *mutex)
{
	if (atomic_get(&mutex->initialized) != 0) {
#ifdef CONFIG_OBJ_CORE_MUTEX
		k_obj_core_unlink(K_OBJ_CORE(&mutex->native));
#endif
		atomic_set(&mutex->initialized, 0);
	}
	return 0;
}

int __libcpp_condvar_signal(__libcpp_condvar_t *cond)
{
	int ret = ensure_condvar(cond);

	if (ret == 0) {
		ret = k_condvar_signal(&cond->native);
		ret = ret < 0 ? -ret : 0;
	}
	return ret;
}

int __libcpp_condvar_broadcast(__libcpp_condvar_t *cond)
{
	int ret = ensure_condvar(cond);

	if (ret == 0) {
		ret = k_condvar_broadcast(&cond->native);
		ret = ret < 0 ? -ret : 0;
	}
	return ret;
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
	if (atomic_get(&cond->initialized) != 0) {
#ifdef CONFIG_OBJ_CORE_CONDVAR
		k_obj_core_unlink(K_OBJ_CORE(&cond->native));
#endif
		atomic_set(&cond->initialized, 0);
	}
	return 0;
}

int __libcpp_execute_once(__libcpp_exec_once_flag *flag, void (*init_routine)())
{
	if (atomic_get(flag) == 2) {
		return 0;
	}
	int ret = k_mutex_lock(&once_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	while (atomic_get(flag) == 1) {
		ret = k_condvar_wait(&once_cond, &once_lock, K_FOREVER);
		if (ret != 0) {
			(void)k_mutex_unlock(&once_lock);
			return -ret;
		}
	}
	if (atomic_get(flag) == 2) {
		return -k_mutex_unlock(&once_lock);
	}
	atomic_set(flag, 1);
	ret = k_mutex_unlock(&once_lock);
	if (ret != 0) {
		return -ret;
	}
#ifndef _LIBCPP_HAS_NO_EXCEPTIONS
	try {
#endif
		init_routine();
#ifndef _LIBCPP_HAS_NO_EXCEPTIONS
	} catch (...) {
		ret = k_mutex_lock(&once_lock, K_FOREVER);
		__ASSERT_NO_MSG(ret == 0);
		atomic_set(flag, 0);
		ret = k_mutex_unlock(&once_lock);
		__ASSERT_NO_MSG(ret == 0);
		(void)k_condvar_broadcast(&once_cond);
		throw;
	}
#endif
	ret = k_mutex_lock(&once_lock, K_FOREVER);
	if (ret != 0) {
		return -ret;
	}
	atomic_set(flag, 2);
	ret = k_mutex_unlock(&once_lock);
	if (ret != 0) {
		return -ret;
	}
	ret = k_condvar_broadcast(&once_cond);
	return ret < 0 ? -ret : 0;
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
	int ret = k_mutex_lock(&thread_pool_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	for (size_t index = 0; index < ARRAY_SIZE(thread_slots); ++index) {
		thread_slot &slot = thread_slots[index];

		if (slot.used && slot.detached && slot.finished &&
		    k_thread_join(&slot.native, K_NO_WAIT) == 0) {
			slot.used = false;
		}
		if (slot.used) {
			continue;
		}
		slot.entry = entry;
		slot.arg = arg;
		slot.used = true;
		slot.detached = false;
		slot.finished = false;
		slot.joining = false;
		*thread = k_thread_create(&slot.native, thread_stacks[index],
					 K_THREAD_STACK_SIZEOF(thread_stacks[index]), thread_entry,
					 &slot, nullptr, nullptr, CONFIG_LIBCXX_THREAD_PRIORITY,
					 0, K_NO_WAIT);
		ret = k_mutex_unlock(&thread_pool_lock);
		return -ret;
	}
	ret = k_mutex_unlock(&thread_pool_lock);
	return ret == 0 ? EAGAIN : -ret;
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
	if (*thread == k_current_get()) {
		return EDEADLK;
	}
	int ret = k_mutex_lock(&thread_pool_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	thread_slot *slot = find_thread(*thread);

	if (slot == nullptr || slot->detached || slot->joining) {
		(void)k_mutex_unlock(&thread_pool_lock);
		return EINVAL;
	}
	slot->joining = true;
	ret = k_mutex_unlock(&thread_pool_lock);
	if (ret != 0) {
		return -ret;
	}
	int join_ret = k_thread_join(&slot->native, K_FOREVER);

	ret = k_mutex_lock(&thread_pool_lock, K_FOREVER);
	if (ret != 0) {
		return -ret;
	}
	slot->joining = false;
	if (join_ret == 0) {
		slot->used = false;
	}
	ret = k_mutex_unlock(&thread_pool_lock);
	return join_ret != 0 ? -join_ret : -ret;
}

int __libcpp_thread_detach(__libcpp_thread_t *thread)
{
	int ret = k_mutex_lock(&thread_pool_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	thread_slot *slot = find_thread(*thread);

	if (slot == nullptr || slot->detached || slot->joining) {
		(void)k_mutex_unlock(&thread_pool_lock);
		return EINVAL;
	}
	slot->detached = true;
	if (slot->finished && k_thread_join(&slot->native, K_NO_WAIT) == 0) {
		slot->used = false;
	}
	return -k_mutex_unlock(&thread_pool_lock);
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
	k_timepoint_t deadline = sys_timepoint_calc(K_NSEC(duration.count()));
	k_timeout_t remaining;

	while (!K_TIMEOUT_EQ(remaining = sys_timepoint_timeout(deadline), K_NO_WAIT)) {
		(void)k_sleep(remaining);
	}
}

int __libcpp_tls_create(__libcpp_tls_key *key, void (*destructor)(void *))
{
	int ret = k_mutex_lock(&initialization_lock, K_FOREVER);

	if (ret != 0) {
		return -ret;
	}
	uint32_t count = static_cast<uint32_t>(atomic_get(&tls_key_count));

	if (count >= CONFIG_LIBCXX_TLS_KEYS) {
		(void)k_mutex_unlock(&initialization_lock);
		return EAGAIN;
	}
	tls_destructors[count] = destructor;
	atomic_set(&tls_key_count, count + 1U);
	*key = count;
	return -k_mutex_unlock(&initialization_lock);
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
