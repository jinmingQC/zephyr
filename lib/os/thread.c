/*
 * Copyright (c) 2026, Friedt Professional Engineering Services, Inc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../../kernel/include/kthread.h"
#include <ksched.h>

#include <zephyr/internal/syscall_handler.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/kernel/thread_stack.h>
#include <zephyr/kernel/internal/mm.h>
#include <zephyr/sys/check.h>
#include <zephyr/sys/internal/kobject_internal.h>
#include <zephyr/sys/thread.h>
#include <zephyr/sys/elastipool.h>
#include <zephyr/sys/hash_map.h>
#include <zephyr/toolchain.h>

#define K_THREAD_STACK_ALIGN(size) Z_KERNEL_STACK_OBJ_ALIGN

/* For declaring an array of k_thread */
#define K_THREAD_ARRAY_DEFINE(name, num) __maybe_unused struct k_thread name[num]

static void *heap_obj_allocator(void *obj, size_t size, enum k_objects otype, uint32_t flags);
static void heap_obj_free(void *obj, bool sched_locked);

LOG_MODULE_REGISTER(sys_thread, CONFIG_SYS_THREAD_LOG_LEVEL);

/* The pool heap allocator does not reuse the caller-supplied storage. */
static inline void *stack_pool_heap_alloc(const void *ptr, size_t size, size_t align,
					  uint32_t flags)
{
	return heap_obj_allocator((void *)ptr, size, K_OBJ_THREAD_STACK_ELEMENT, flags);
}

static inline void *thread_pool_heap_alloc(const void *ptr, size_t size, size_t align,
					   uint32_t flags)
{
	return heap_obj_allocator((void *)ptr, size, K_OBJ_THREAD, flags);
}

static const char *const heap_obj_str(enum k_objects otype);

BUILD_ASSERT(SYS_THREAD_STACK_MIN >= 0);
BUILD_ASSERT(CONFIG_SYS_THREAD_STACK_MAX >= SYS_THREAD_STACK_MIN);

/* Reserve elastic entries under the pool lock, allocate without it, then register
 * the allocation under the lock.
 */
static struct k_spinlock stack_pool_lock;
K_THREAD_STACK_ARRAY_DEFINE(sys_stack_pool, SYS_THREAD_STACK_MIN, CONFIG_SYS_THREAD_STACK_SIZE);
SYS_ELASTIPOOL_DEFINE_ADVANCED(stack_pool, K_THREAD_STACK_LEN(CONFIG_SYS_THREAD_STACK_SIZE),
			       K_THREAD_STACK_ALIGN(CONFIG_SYS_THREAD_STACK_SIZE),
			       SYS_THREAD_STACK_MIN, CONFIG_SYS_THREAD_STACK_MAX,
			       stack_pool_heap_alloc, sys_stack_pool, static);

BUILD_ASSERT(SYS_THREAD_THREAD_MIN >= 0);
BUILD_ASSERT(CONFIG_SYS_THREAD_THREAD_MAX >= SYS_THREAD_THREAD_MIN);

static struct k_spinlock thread_pool_lock;
K_THREAD_ARRAY_DEFINE(sys_thread_pool, SYS_THREAD_THREAD_MIN);
SYS_ELASTIPOOL_DEFINE_ADVANCED(thread_pool, sizeof(struct k_thread), __alignof(struct k_thread),
			       SYS_THREAD_THREAD_MIN, CONFIG_SYS_THREAD_THREAD_MAX,
			       thread_pool_heap_alloc, sys_thread_pool, static);

/*
 * How an elastic stack was obtained, kept in the pool's map so that the release
 * path never has to ask the kernel-object layer (whose locks it may be under).
 */
#define STACK_KIND_HEAP   0U /* heap block, not a kernel object */
#define STACK_KIND_MAPPED 2U /* anonymous mapping, not a kernel object */
#define STACK_KIND_MASK   3U
#define STACK_VALUE(kind, size) ((uint64_t)(size) << 2 | (kind))
#define STACK_KIND(value) ((uint32_t)((value) & STACK_KIND_MASK))
#define STACK_SIZE(value) ((size_t)((value) >> 2))

static bool pool_is_elastic(const struct sys_elastipool *pool)
{
	return (pool->config->map != NULL) && (pool->config->max_obj > pool->config->min_obj);
}

/* lock held: reserve an elastic slot */
static int pool_reserve_locked(const struct sys_elastipool *pool)
{
	if (!pool_is_elastic(pool) || (pool->data->pool_size >= pool->config->max_obj)) {
		return -ENOMEM;
	}
	++pool->data->pool_size;

	return 0;
}

/* lock held: register an allocation in a reserved slot, or give the slot back */
static int pool_register_locked(const struct sys_elastipool *pool, const void *obj, uint64_t value)
{
	if ((obj == NULL) || (sys_hashmap_insert(pool->config->map, (uint64_t)(uintptr_t)obj,
						  value, NULL) < 0)) {
		--pool->data->pool_size;
		return -ENOMEM;
	}

	return 0;
}

/* lock held: drop the bookkeeping of a pool entry; its map value when elastic */
static int pool_forget_locked(const struct sys_elastipool *pool, const void *obj, uint64_t *value)
{
	if ((pool->config->min_obj > 0) && (sys_elastipool_api_free_static(pool, obj) == 0)) {
		return 1;
	}

	if (pool_is_elastic(pool) &&
	    sys_hashmap_remove(pool->config->map, (uint64_t)(uintptr_t)obj, value)) {
		--pool->data->pool_size;
		return 0;
	}

	return -EINVAL;
}

static void stack_free_memory(void *stack, uint64_t value, bool sched_locked)
{
	switch (STACK_KIND(value)) {
	case STACK_KIND_HEAP:
		heap_obj_free(stack, sched_locked);
		break;
#ifdef CONFIG_MMU
	case STACK_KIND_MAPPED:
		k_mem_unmap(stack, STACK_SIZE(value));
		break;
#endif /* CONFIG_MMU */
	default:
		/* a kernel object: the collector frees it */
		break;
	}
}

/*
 * Give a pool stack back: bookkeeping, and the memory when the pool allocated
 * it and it is not a kernel object (the collector frees those on its own).
 */
static int stack_pool_release(void *stack, bool sched_locked)
{
	int ret;
	uint64_t value = 0;

	K_SPINLOCK(&stack_pool_lock) {
		ret = pool_forget_locked(&stack_pool, stack, &value);
	}
	if (ret < 0) {
		return ret;
	}
	if (ret == 0) {
		stack_free_memory(stack, value, sched_locked);
	}

	return 0;
}

/* the thread object leaves the pool: bookkeeping, and its memory when asked */
static int thread_pool_release(struct k_thread *th, bool free_memory, bool sched_locked)
{
	int ret;
	uint64_t value = 0;

	K_SPINLOCK(&thread_pool_lock) {
		ret = pool_forget_locked(&thread_pool, th, &value);
	}
	if (ret < 0) {
		return ret;
	}
	if ((ret == 0) && free_memory) {
		heap_obj_free(th, sched_locked);
	}

	return 0;
}

/* heap_obj_allocator() hands out a kernel object for these */

bool z_sys_thread_owned(const struct k_thread *thread)
{
	bool owned;

	K_SPINLOCK(&thread_pool_lock) {
		owned = (sys_elastipool_check(&thread_pool, thread) == 0);
	}

	return owned;
}

int z_sys_thread_reclaim(struct k_thread *thread)
{
	if (thread->stack_obj != NULL) {
		(void)stack_pool_release(thread->stack_obj, false);
	}

	return thread_pool_release(thread, true, false);
}

static int stack_pool_draw(k_thread_stack_t **stack, uint32_t *stack_size, uint32_t flags)
{
	int ret = -ENOMEM;
	bool elastic = false;

	if (*stack_size < CONFIG_SYS_THREAD_STACK_SIZE) {
		*stack_size = CONFIG_SYS_THREAD_STACK_SIZE;
	}

	K_SPINLOCK(&stack_pool_lock) {
		if ((*stack_size <= CONFIG_SYS_THREAD_STACK_SIZE) && (SYS_THREAD_STACK_MIN > 0)) {
			/* Try the guaranteed-static entries first */
			ret = sys_elastipool_api_alloc_static(&stack_pool, (void **)stack, flags);
			if (ret == 0) {
				K_SPINLOCK_BREAK;
			}
		}
		ret = pool_reserve_locked(&stack_pool);
		elastic = (ret == 0);
	}

	if (ret < 0) {
		return ret;
	}
	if (!elastic) {
		return 0;
	}

	uint64_t value;

	if (IS_ENABLED(CONFIG_SYS_THREAD_STACK_ALLOC_ANON) && IS_ENABLED(CONFIG_MMU)) {
#ifdef CONFIG_MMU
		size_t map_size = ROUND_UP(K_KERNEL_STACK_LEN(*stack_size), CONFIG_MMU_PAGE_SIZE);

		*stack = k_mem_map(map_size, K_MEM_PERM_RW | K_MEM_MAP_LOCK | K_MEM_MAP_UNINIT);
		value = STACK_VALUE(STACK_KIND_MAPPED, map_size);
#endif /* CONFIG_MMU */
	} else if (IS_ENABLED(CONFIG_SYS_THREAD_STACK_ALLOC_HEAP)) {
		/* Forward flags so K_USER reaches the heap allocator: a user
		 * thread must not be given a kernel-only stack object.
		 */
		*stack = heap_obj_allocator(NULL, *stack_size, K_OBJ_THREAD_STACK_ELEMENT, flags);
		value = STACK_VALUE(STACK_KIND_HEAP, 0);
	} else {
		__ASSERT(false, "Invalid CONFIG_SYS_THREAD_STACK_ALLOC");
		*stack = NULL;
		value = 0;
	}

	K_SPINLOCK(&stack_pool_lock) {
		ret = pool_register_locked(&stack_pool, *stack, value);
	}
	if (ret < 0) {
		if (*stack != NULL) {
			stack_free_memory(*stack, value, false);
			*stack = NULL;
		}
		return ret;
	}

	return 0;
}

int z_impl_sys_thread_stack_alloc(k_thread_stack_t **stack, uint32_t *stack_size,
				  uint32_t guard_size, uint32_t flags)
{
	ARG_UNUSED(guard_size);

	if (CONFIG_SYS_THREAD_STACK_MAX == 0) {
		LOG_WRN_ONCE("%s pool is disabled", heap_obj_str(K_OBJ_THREAD_STACK_ELEMENT));
		return -ENOTSUP;
	}

	if ((*stack_size > CONFIG_SYS_THREAD_STACK_SIZE) &&
	    (CONFIG_SYS_THREAD_STACK_MAX == SYS_THREAD_STACK_MIN)) {
		/*
		 * A static pool cannot satisfy a request for a larger stack.
		 */
		LOG_WRN_ONCE("%s of size %u exceeds CONFIG_SYS_THREAD_STACK_SIZE (%u)",
			     heap_obj_str(K_OBJ_THREAD_STACK_ELEMENT), *stack_size,
			     CONFIG_SYS_THREAD_STACK_SIZE);
		return -ENOMEM;
	}

	return stack_pool_draw(stack, stack_size, flags);
}

struct stack_user_cb_data {
	k_tid_t tid;
	k_thread_stack_t *stack;
};

static void stack_user_cb(const struct k_thread *thread, void *user_data)
{
	struct stack_user_cb_data *const data = (struct stack_user_cb_data *)user_data;

	if (data->stack == (k_thread_stack_t *)thread->stack_info.start) {
		data->tid = (k_tid_t)thread;
	}
}

/* a stack that is not a kernel object has no liveness flag: ask the threads */
static bool stack_in_use(k_thread_stack_t *stack)
{
	struct stack_user_cb_data data = {.stack = stack};

	k_thread_foreach(stack_user_cb, &data);

	return (data.tid != NULL) && !(z_is_thread_state_set(data.tid, _THREAD_DUMMY) ||
				       z_is_thread_state_set(data.tid, _THREAD_DEAD));
}

int z_impl_sys_thread_stack_destroy(k_thread_stack_t *stack)
{
	if (CONFIG_SYS_THREAD_STACK_MAX == 0) {
		LOG_WRN_ONCE("%s pool is disabled", heap_obj_str(K_OBJ_THREAD_STACK_ELEMENT));
		return -ENOTSUP;
	}

	CHECKIF(stack_in_use(stack)) {
		return -EBUSY;
	}

	return stack_pool_release(stack, false);
}

static int thread_pool_draw(struct k_thread **th)
{
	int ret = -ENOMEM;
	bool elastic = false;

	K_SPINLOCK(&thread_pool_lock) {
		if (SYS_THREAD_THREAD_MIN > 0) {
			ret = sys_elastipool_api_alloc_static(&thread_pool, (void **)th, 0);
			if (ret == 0) {
				K_SPINLOCK_BREAK;
			}
		}
		ret = pool_reserve_locked(&thread_pool);
		elastic = (ret == 0);
	}

	if (ret < 0) {
		return ret;
	}
	if (elastic) {
		*th = heap_obj_allocator(NULL, 0, K_OBJ_THREAD, 0);
		K_SPINLOCK(&thread_pool_lock) {
			ret = pool_register_locked(&thread_pool, *th, 0);
		}
		if (ret < 0) {
			if (*th != NULL) {
				heap_obj_free(*th, false);
				*th = NULL;
			}
			return ret;
		}
	}
	/* nothing may be read from it before k_thread_create() */
	(*th)->stack_obj = NULL;

	return 0;
}

int z_impl_sys_thread_create(struct k_thread **th, k_thread_stack_t *stack, uint32_t stack_size,
			     uint32_t guard_size, k_thread_entry_t entry, void *p1, void *p2,
			     void *p3, int prio, uint32_t options)
{
	int ret;

	/* deferred disposals of dead threads come back to the pools first */
	z_thread_graveyard_flush();

	ret = thread_pool_draw(th);
	if (ret < 0) {
		return ret;
	}

	if (stack == NULL) {
		ret = sys_thread_stack_alloc(&stack, &stack_size, guard_size, options);
		if (ret < 0) {
			(void)thread_pool_release(*th, true, false);
			return ret;
		}
		/* sys_thread_stack_alloc() normalizes stack_size up to at least
		 * CONFIG_SYS_THREAD_STACK_SIZE; reflect that here so k_thread_create()
		 * receives the correct usable size.
		 */
		if (stack_size < CONFIG_SYS_THREAD_STACK_SIZE) {
			stack_size = CONFIG_SYS_THREAD_STACK_SIZE;
		}
	}

	k_thread_create(*th, stack, stack_size, entry, p1, p2, p3, prio, options, K_NO_WAIT);

	return 0;
}

int z_impl_sys_thread_destroy(struct k_thread *th)
{
	if ((CONFIG_SYS_THREAD_STACK_MAX == 0) && (CONFIG_SYS_THREAD_THREAD_MAX == 0)) {
		LOG_WRN_ONCE("%s and %s pool are disabled", heap_obj_str(K_OBJ_THREAD),
			     heap_obj_str(K_OBJ_THREAD_STACK_ELEMENT));
		return -ENOTSUP;
	}

	CHECKIF(!(z_is_thread_state_set(th, _THREAD_DUMMY) ||
		  z_is_thread_state_set(th, _THREAD_DEAD))) {
		return -EBUSY;
	}

	z_thread_reap(th);

	return 0;
}

static inline const char *const heap_obj_str(enum k_objects otype)
{
	switch (otype) {
	case K_OBJ_THREAD_STACK_ELEMENT:
		return "k_thread_stack_t";
	case K_OBJ_THREAD:
		return "k_thread";
	default:
		__ASSERT(false, "invalid object type %d", otype);
	}

	return "";
}

static void *heap_obj_allocator(void *obj, size_t size, enum k_objects otype, uint32_t flags)
{
	__ASSERT(obj == NULL, "the pool only allocates here");

	ARG_UNUSED(flags);

	size_t align;

	switch (otype) {
	case K_OBJ_THREAD:
		size = sizeof(struct k_thread);
		align = __alignof(struct k_thread);
		break;
	case K_OBJ_THREAD_STACK_ELEMENT:
		size = K_KERNEL_STACK_LEN(size);
		align = Z_KERNEL_STACK_OBJ_ALIGN;
		break;
	default:
		__ASSERT(false, "invalid object type %d", otype);
		return NULL;
	}

	return z_thread_aligned_alloc(align, size);
}

/* memory the pool allocated and that is not a kernel object */
static void heap_obj_free(void *obj, bool sched_locked)
{
	__ASSERT(!sched_locked, "heap release under the scheduler lock");
	k_free(obj);
}

enum {
	ONCE_RUNNING = 1,
	ONCE_DONE,
};

static K_MUTEX_DEFINE(once_lock);
static K_CONDVAR_DEFINE(once_done);

void sys_thread_once(sys_thread_once_t *flag, void (*func)(void))
{
	if (atomic_get(&flag->val) == ONCE_DONE) {
		return;
	}

	if (atomic_cas(&flag->val, 0, ONCE_RUNNING)) {
		func();

		k_mutex_lock(&once_lock, K_FOREVER);
		atomic_set(&flag->val, ONCE_DONE);
		k_condvar_broadcast(&once_done);
		k_mutex_unlock(&once_lock);
		return;
	}

	k_mutex_lock(&once_lock, K_FOREVER);
	while (atomic_get(&flag->val) != ONCE_DONE) {
		k_condvar_wait(&once_done, &once_lock, K_FOREVER);
	}
	k_mutex_unlock(&once_lock);
}

#ifdef CONFIG_ZTEST
#include <zephyr/ztest.h>

/* pool usage before each test: what the test must leave behind */
static size_t pool_baseline[2];
static const struct sys_elastipool *const pools[] = {&stack_pool, &thread_pool};
static const enum k_objects pool_types[] = {K_OBJ_THREAD_STACK_ELEMENT, K_OBJ_THREAD};

static void record_stats(const struct ztest_unit_test *test, void *data)
{
	ARG_UNUSED(test);
	ARG_UNUSED(data);

	ARRAY_FOR_EACH(pools, i) {
		pool_baseline[i] = pools[i]->data->pool_size;
	}
}

static bool pools_at_baseline(void)
{
	ARRAY_FOR_EACH(pools, i) {
		if (pools[i]->data->pool_size != pool_baseline[i]) {
			return false;
		}
	}

	return true;
}

/*
 * Only observes: a thread that exited on another CPU may still be halting, and
 * its objects come back once it has switched out, so the pools get a bounded
 * time to settle before a leak is called.
 */
static void print_stats(const struct ztest_unit_test *test, void *data)
{
	ARG_UNUSED(test);
	ARG_UNUSED(data);

	for (uint32_t t0 = k_uptime_get_32(); !pools_at_baseline();) {
		z_thread_graveyard_flush();
		if (pools_at_baseline() || ((k_uptime_get_32() - t0) > 200U)) {
			break;
		}
		k_msleep(1);
	}

	bool leak = !pools_at_baseline();

	ARRAY_FOR_EACH(pools, i) {
		if (pools[i]->data->pool_size != pool_baseline[i]) {
			TC_PRINT("%s pool usage: %zu/%zu (was %zu)\n", heap_obj_str(pool_types[i]),
				 pools[i]->data->pool_size, pools[i]->config->max_obj,
				 pool_baseline[i]);
		}
	}

	/* This will assert if any tests leak objects */
	__ASSERT(!leak, "object pool leak detected");
}

ZTEST_RULE(sys_thread_rule, record_stats, print_stats);
#endif /* CONFIG_ZTEST */
