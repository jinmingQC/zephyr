// SPDX-License-Identifier: Apache-2.0
/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors */

/*
 * Privileged half of the native libc++ adapter. Keep this translation unit
 * OUT of cpp_runtime_partition: users may only modify the small handles.
 */
#include <cpp_runtime/native_syscalls.h>
#include <cpp_runtime/userspace.h>
#include <cpp_runtime/partition.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/sys/util.h>

/* The descriptor stays protected; only the linked partition is user memory. */
K_APPMEM_PARTITION_DEFINE(cpp_runtime_partition);

BUILD_ASSERT(IS_ENABLED(CONFIG_USERSPACE));
BUILD_ASSERT(CONFIG_CPP_RUNTIME_MUTEX_MAX > 0 && CONFIG_CPP_RUNTIME_MUTEX_MAX <= 255);
BUILD_ASSERT(CONFIG_CPP_RUNTIME_CONDVAR_MAX > 0 && CONFIG_CPP_RUNTIME_CONDVAR_MAX <= 255);

/* Low eight bits hold index + 1, the next bit distinguishes object types. */
#define HANDLE_COND_BIT BIT64(8)
#define HANDLE_GEN_SHIFT 9U
#define HANDLE_GEN_MAX (UINT64_MAX >> HANDLE_GEN_SHIFT)

struct sync_slot {
	uint64_t *address;
	uint64_t generation;
	unsigned int refs;
	bool used;
};

struct runtime_thread {
	struct k_mem_domain *domain;
	bool used;
	bool detached;
	bool joining;
	bool user;
};

static K_MUTEX_DEFINE(sync_lock);
static K_MUTEX_DEFINE(thread_lock);
static K_MUTEX_DEFINE(domain_lock);

/* Separate arrays make every native object discoverable by Zephyr's scanner. */
static struct k_mutex mutexes[CONFIG_CPP_RUNTIME_MUTEX_MAX];
static struct k_condvar condvars[CONFIG_CPP_RUNTIME_CONDVAR_MAX];
static struct sync_slot mutex_slots[CONFIG_CPP_RUNTIME_MUTEX_MAX];
static struct sync_slot cond_slots[CONFIG_CPP_RUNTIME_CONDVAR_MAX];

static struct k_thread threads[CONFIG_CPP_RUNTIME_THREAD_MAX];
static K_THREAD_STACK_ARRAY_DEFINE(stacks, CONFIG_CPP_RUNTIME_THREAD_MAX,
				   CONFIG_CPP_RUNTIME_THREAD_STACK_SIZE);
static struct runtime_thread thread_slots[CONFIG_CPP_RUNTIME_THREAD_MAX];

/* Published before user threads become runnable; never changed afterwards. */
static atomic_ptr_t application_domain;

static int caller_allowed(bool user)
{
	struct k_mem_domain *domain = atomic_ptr_get(&application_domain);

	if (k_is_in_isr()) {
		return -EPERM;
	}
	if (user && (domain == NULL ||
		     k_current_get()->mem_domain_info.mem_domain != domain)) {
		return -EACCES;
	}
	return 0;
}

static bool in_runtime_partition(const void *pointer)
{
	uintptr_t address = (uintptr_t)pointer;

	return address >= cpp_runtime_partition.start &&
	       address - cpp_runtime_partition.start < cpp_runtime_partition.size;
}

int cpp_runtime_userspace_init(struct k_mem_domain *domain)
{
	struct k_mem_domain *registered;
	int ret;

	if (k_is_user_context() || k_is_in_isr()) {
		return -EPERM;
	}
	if (domain == NULL) {
		return -EINVAL;
	}
	ret = k_mutex_lock(&domain_lock, K_FOREVER);
	if (ret != 0) {
		return ret;
	}
	registered = atomic_ptr_get(&application_domain);
	if (registered != NULL) {
		ret = registered == domain ? 0 : -EALREADY;
	} else {
		/* Fail closed if a linker failed to place ordinary archive data
		 * into the user partition, or accidentally exposed kernel pools.
		 */
		if (!in_runtime_partition(&cpp_runtime_data_anchor) ||
		    !in_runtime_partition(&cpp_runtime_bss_anchor) ||
		    in_runtime_partition(mutexes) || in_runtime_partition(condvars) ||
		    in_runtime_partition(threads) || in_runtime_partition(thread_slots)) {
			ret = -ENOEXEC;
		} else {
			ret = k_mem_domain_add_partition(domain, &cpp_runtime_partition);
		}
		if (ret == 0) {
			atomic_ptr_set(&application_domain, domain);
		}
	}
	(void)k_mutex_unlock(&domain_lock);
	return ret;
}

static int read_handle(uint64_t *address, uint64_t *value, bool user)
{
	if (address == NULL || (uintptr_t)address % _Alignof(uint64_t) != 0U) {
		return -EINVAL;
	}
	if (user) {
		/* All operations require a writable handle, even pure notifications. */
		if (K_SYSCALL_MEMORY_WRITE(address, sizeof(*address)) ||
		    k_usermode_from_copy(value, address, sizeof(*value)) != 0) {
			return -EFAULT;
		}
	} else {
		*value = *address;
	}
	return 0;
}

static int write_handle(uint64_t *address, uint64_t value, bool user)
{
	if (user) {
		return k_usermode_to_copy(address, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
	}
	*address = value;
	return 0;
}

static uint64_t slot_token(const struct sync_slot *slot, size_t index, bool cond)
{
	return (slot->generation << HANDLE_GEN_SHIFT) | (uint64_t)(index + 1U) |
	       (cond ? HANDLE_COND_BIT : 0U);
}

static void unlink_sync_object(size_t index, bool cond)
{
	if (cond) {
#ifdef CONFIG_OBJ_CORE_CONDVAR
		k_obj_core_unlink(K_OBJ_CORE(&condvars[index]));
#endif
	} else {
#ifdef CONFIG_OBJ_CORE_MUTEX
		k_obj_core_unlink(K_OBJ_CORE(&mutexes[index]));
#endif
	}
}

/*
 * Caller holds sync_lock. Address binding is authoritative: zeroing or
 * replacing a live handle must not allocate another slot for the same object.
 * Generation values never wrap; an exhausted slot is permanently retired.
 */
static int resolve_slot(uint64_t *address, bool cond, bool create, bool user,
			size_t *index)
{
	struct sync_slot *slots = cond ? cond_slots : mutex_slots;
	const size_t count = cond ? ARRAY_SIZE(cond_slots) : ARRAY_SIZE(mutex_slots);
	struct sync_slot *opposite = cond ? mutex_slots : cond_slots;
	const size_t opposite_count = cond ? ARRAY_SIZE(mutex_slots) : ARRAY_SIZE(cond_slots);
	uint64_t token;
	int ret = read_handle(address, &token, user);

	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0; i < opposite_count; ++i) {
		if (opposite[i].used && opposite[i].address == address) {
			return -EINVAL;
		}
	}
	for (size_t i = 0; i < count; ++i) {
		if (slots[i].used && slots[i].address == address) {
			if (token != slot_token(&slots[i], i, cond)) {
				return -EINVAL;
			}
			*index = i;
			return 0;
		}
	}
	if (token != 0U) {
		/* Reject stale, forged, wrong-type, and copied-to-another-address handles. */
		return -EINVAL;
	}
	if (!create) {
		return -ENOENT;
	}
	for (size_t i = 0; i < count; ++i) {
		struct sync_slot *slot = &slots[i];

		if (slot->used || slot->generation == HANDLE_GEN_MAX) {
			continue;
		}
		ret = cond ? k_condvar_init(&condvars[i]) : k_mutex_init(&mutexes[i]);
		if (ret != 0) {
			return ret;
		}
		++slot->generation;
		slot->address = address;
		slot->refs = 0U;
		slot->used = true;
		ret = write_handle(address, slot_token(slot, i, cond), user);
		if (ret != 0) {
			slot->used = false;
			slot->address = NULL;
			unlink_sync_object(i, cond);
			return ret;
		}
		*index = i;
		return 0;
	}
	return -ENOMEM;
}

static int destroy_slot(uint64_t *address, bool cond, bool user)
{
	struct sync_slot *slots = cond ? cond_slots : mutex_slots;
	size_t index;
	int ret = resolve_slot(address, cond, false, user, &index);

	if (ret == -ENOENT) {
		return 0; /* An untouched zero-initialized object owns no kernel slot. */
	}
	if (ret != 0) {
		return ret;
	}
	if (slots[index].refs != 0U || (!cond && mutexes[index].lock_count != 0U)) {
		return -EBUSY;
	}
	ret = write_handle(address, 0U, user);
	if (ret != 0) {
		return ret;
	}
	unlink_sync_object(index, cond);
	slots[index].address = NULL;
	slots[index].used = false;
	return 0;
}

static int runtime_sync(uint64_t *handle, unsigned int op, uint64_t *other,
			int64_t timeout_ticks, bool user)
{
	const bool cond = op >= CPP_RUNTIME_COND_SIGNAL;
	size_t index = 0U;
	size_t mutex_index = 0U;
	struct sync_slot *slot;
	int ret = caller_allowed(user);

	if (ret != 0) {
		return ret;
	}
	if (op > CPP_RUNTIME_COND_DESTROY || timeout_ticks < -1) {
		return -EINVAL;
	}
	/*
	 * The user adapter chunks long deadlines into one-day relative waits.
	 * Keep hostile values away from the scheduler's timeout + elapsed + 1
	 * arithmetic, including INT64_MAX with CONFIG_TIMEOUT_64BIT.
	 */
	if (timeout_ticks > (int64_t)k_sec_to_ticks_ceil64(86400) ||
	    (timeout_ticks >= 0 &&
	     (uint64_t)timeout_ticks > (uint64_t)K_TICKS_FOREVER - 1U)) {
		return -EINVAL;
	}
	ret = k_mutex_lock(&sync_lock, K_FOREVER);
	if (ret != 0) {
		return ret;
	}
	if (op == CPP_RUNTIME_MUTEX_DESTROY || op == CPP_RUNTIME_COND_DESTROY) {
		ret = destroy_slot(handle, cond, user);
		goto unlock;
	}
	if (op == CPP_RUNTIME_COND_WAIT) {
		ret = resolve_slot(other, false, false, user, &mutex_index);
		if (ret != 0) {
			goto unlock;
		}
		/* Never let an invalid caller unlock someone else's mutex. */
		if (mutexes[mutex_index].owner != k_current_get()) {
			ret = -EPERM;
			goto unlock;
		}
		if (mutexes[mutex_index].lock_count != 1U) {
			ret = -EINVAL;
			goto unlock;
		}
		if (mutex_slots[mutex_index].refs == UINT_MAX) {
			ret = -EOVERFLOW;
			goto unlock;
		}
	}
	ret = resolve_slot(handle, cond, op != CPP_RUNTIME_MUTEX_UNLOCK, user, &index);
	if (ret != 0) {
		ret = ret == -ENOENT ? -EINVAL : ret;
		goto unlock;
	}
	slot = cond ? &cond_slots[index] : &mutex_slots[index];
	if (op == CPP_RUNTIME_MUTEX_INIT) {
		goto unlock;
	}
	if (slot->refs == UINT_MAX) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	++slot->refs;
	if (op == CPP_RUNTIME_COND_WAIT) {
		++mutex_slots[mutex_index].refs;
	}
	(void)k_mutex_unlock(&sync_lock);

	/*
	 * Keep both objects pinned while the native operation can block. No
	 * userspace pointer is followed here and no user callback is invoked.
	 */
	switch (op) {
	case CPP_RUNTIME_MUTEX_LOCK:
		ret = k_mutex_lock(&mutexes[index], K_FOREVER);
		break;
	case CPP_RUNTIME_MUTEX_TRYLOCK:
		ret = k_mutex_lock(&mutexes[index], K_NO_WAIT);
		break;
	case CPP_RUNTIME_MUTEX_UNLOCK:
		ret = k_mutex_unlock(&mutexes[index]);
		break;
	case CPP_RUNTIME_COND_SIGNAL:
		ret = k_condvar_signal(&condvars[index]);
		break;
	case CPP_RUNTIME_COND_BROADCAST:
		ret = k_condvar_broadcast(&condvars[index]);
		if (ret >= 0) {
			ret = 0;
		}
		break;
	case CPP_RUNTIME_COND_WAIT: {
		k_timeout_t timeout = timeout_ticks < 0 ? K_FOREVER : K_TICKS(timeout_ticks);

		ret = k_condvar_wait(&condvars[index], &mutexes[mutex_index], timeout);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	(void)k_mutex_lock(&sync_lock, K_FOREVER);
	--slot->refs;
	if (op == CPP_RUNTIME_COND_WAIT) {
		--mutex_slots[mutex_index].refs;
	}
unlock:
	(void)k_mutex_unlock(&sync_lock);
	return ret;
}

int z_impl_cpp_runtime_sync(uint64_t *handle, unsigned int op, uint64_t *other,
			   int64_t timeout_ticks)
{
	return runtime_sync(handle, op, other, timeout_ticks, false);
}

static inline int z_vrfy_cpp_runtime_sync(uint64_t *handle, unsigned int op,
					uint64_t *other, int64_t timeout_ticks)
{
	return runtime_sync(handle, op, other, timeout_ticks, true);
}
#include <zephyr/syscalls/cpp_runtime_sync_mrsh.c>

/* Static objects are never freed when permissions are cleared. */
static void clear_object_permissions(void *object)
{
	k_object_access_revoke_others(object);
	k_object_access_revoke(object, k_current_get());
}

static void release_thread_slot(size_t index)
{
	clear_object_permissions(&threads[index]);
	clear_object_permissions(stacks[index]);
	thread_slots[index].used = false;
	thread_slots[index].domain = NULL;
}

static int find_thread_slot(k_tid_t thread, bool user, size_t *index)
{
	for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
		if (thread == &threads[i] && thread_slots[i].used) {
			if (user && (!thread_slots[i].user ||
				     thread_slots[i].domain != atomic_ptr_get(&application_domain))) {
				return -EACCES;
			}
			*index = i;
			return 0;
		}
	}
	return -EINVAL;
}

static int runtime_thread_create(k_tid_t *result, cpp_runtime_entry_t entry,
				 void *arg, bool user)
{
	int ret = caller_allowed(user);

	if (ret != 0) {
		return ret;
	}
	if (result == NULL || entry == NULL ||
	    (uintptr_t)result % _Alignof(k_tid_t) != 0U) {
		return -EINVAL;
	}
	if (user && K_SYSCALL_MEMORY_WRITE(result, sizeof(*result))) {
		return -EFAULT;
	}
	ret = k_mutex_lock(&thread_lock, K_FOREVER);
	if (ret != 0) {
		return ret;
	}
	ret = -EAGAIN;
	for (size_t i = 0; i < ARRAY_SIZE(threads); ++i) {
		struct runtime_thread *slot = &thread_slots[i];
		k_tid_t created;
		int priority = CONFIG_CPP_RUNTIME_THREAD_PRIORITY;
		uint32_t options = 0U;

		/* A finished notification is insufficient: join proves CPU quiescence. */
		if (slot->used && slot->detached &&
		    k_thread_join(&threads[i], K_NO_WAIT) == 0) {
			release_thread_slot(i);
		}
		if (slot->used) {
			continue;
		}
		clear_object_permissions(&threads[i]);
		clear_object_permissions(stacks[i]);
		if (user) {
			options = K_USER | K_INHERIT_PERMS;
			priority = MAX(priority, k_thread_priority_get(k_current_get()));
		}
		slot->used = true;
		slot->detached = false;
		slot->joining = false;
		slot->user = user;
		slot->domain = k_current_get()->mem_domain_info.mem_domain;
		created = k_thread_create(&threads[i], stacks[i], K_THREAD_STACK_SIZEOF(stacks[i]),
					  cpp_runtime_thread_entry, (void *)entry, arg, NULL,
					  priority, options, K_FOREVER);
		/*
		 * The stack remains private. Access to the thread's native handle
		 * permits the creator to use normal, checked Zephyr thread APIs.
		 */
		k_object_access_grant(created, k_current_get());
		if (user) {
			ret = k_usermode_to_copy(result, &created, sizeof(created)) == 0 ?
			      0 : -EFAULT;
		} else {
			*result = created;
			ret = 0;
		}
		if (ret != 0) {
			k_thread_abort(created);
			release_thread_slot(i);
			break;
		}
		k_thread_start(created);
		break;
	}
	(void)k_mutex_unlock(&thread_lock);
	return ret;
}

int z_impl_cpp_runtime_thread_create(k_tid_t *thread, cpp_runtime_entry_t entry, void *arg)
{
	return runtime_thread_create(thread, entry, arg, false);
}

static inline int z_vrfy_cpp_runtime_thread_create(k_tid_t *thread, cpp_runtime_entry_t entry,
						 void *arg)
{
	return runtime_thread_create(thread, entry, arg, true);
}
#include <zephyr/syscalls/cpp_runtime_thread_create_mrsh.c>

static int runtime_thread_join(k_tid_t thread, bool user)
{
	size_t index;
	int ret = caller_allowed(user);

	if (ret != 0) {
		return ret;
	}
	if (thread == k_current_get()) {
		return -EDEADLK;
	}
	ret = k_mutex_lock(&thread_lock, K_FOREVER);
	if (ret != 0) {
		return ret;
	}
	ret = find_thread_slot(thread, user, &index);
	if (ret == 0 && (thread_slots[index].joining || thread_slots[index].detached)) {
		ret = -EINVAL;
	}
	if (ret != 0) {
		(void)k_mutex_unlock(&thread_lock);
		return ret;
	}
	thread_slots[index].joining = true;
	(void)k_mutex_unlock(&thread_lock);

	ret = k_thread_join(&threads[index], K_FOREVER);

	(void)k_mutex_lock(&thread_lock, K_FOREVER);
	thread_slots[index].joining = false;
	if (ret == 0) {
		release_thread_slot(index);
	}
	(void)k_mutex_unlock(&thread_lock);
	return ret;
}

int z_impl_cpp_runtime_thread_join(k_tid_t thread)
{
	return runtime_thread_join(thread, false);
}

static inline int z_vrfy_cpp_runtime_thread_join(k_tid_t thread)
{
	return runtime_thread_join(thread, true);
}
#include <zephyr/syscalls/cpp_runtime_thread_join_mrsh.c>

static int runtime_thread_detach(k_tid_t thread, bool user)
{
	size_t index;
	int ret = caller_allowed(user);

	if (ret != 0) {
		return ret;
	}
	ret = k_mutex_lock(&thread_lock, K_FOREVER);
	if (ret != 0) {
		return ret;
	}
	ret = find_thread_slot(thread, user, &index);
	if (ret == 0 && (thread_slots[index].joining || thread_slots[index].detached)) {
		ret = -EINVAL;
	}
	if (ret == 0) {
		thread_slots[index].detached = true;
		if (k_thread_join(&threads[index], K_NO_WAIT) == 0) {
			release_thread_slot(index);
		}
	}
	(void)k_mutex_unlock(&thread_lock);
	return ret;
}

int z_impl_cpp_runtime_thread_detach(k_tid_t thread)
{
	return runtime_thread_detach(thread, false);
}

static inline int z_vrfy_cpp_runtime_thread_detach(k_tid_t thread)
{
	return runtime_thread_detach(thread, true);
}
#include <zephyr/syscalls/cpp_runtime_thread_detach_mrsh.c>
