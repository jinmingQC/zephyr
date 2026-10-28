// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/posix/pthread.h>
#include <zephyr/sys/atomic.h>

/*
 * libc++ continues to use its unmodified pthread backend.  These wrappers
 * supply the C++ language TLS exit phase, which precedes pthread key
 * destructors (including libc++'s notify_all_at_thread_exit state).
 */
struct start_context {
	void *(*entry)(void *entry_argument);
	void *argument;
};

struct language_tls_dtor {
	void (*function)(void *dtor_object);
	void *object;
	struct language_tls_dtor *next;
};

static __thread struct language_tls_dtor *language_tls_dtors;
static __thread pthread_t current_cpp_id;
static atomic_t next_native_id = ATOMIC_INIT(0);

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
				void *(*entry)(void *entry_argument), void *argument);
pthread_t __real_pthread_self(void);
FUNC_NORETURN void __real_pthread_exit(void *value);

static void run_language_tls_dtors(void)
{
	while (language_tls_dtors != NULL) {
		struct language_tls_dtor *item = language_tls_dtors;
		void (*function)(void *dtor_object) = item->function;
		void *object = item->object;

		language_tls_dtors = item->next;
		free(item);
		/* A destructor may register another destructor. */
		function(object);
	}
}

int __cxa_thread_atexit_impl(void (*function)(void *dtor_object), void *object, void *dso_handle)
{
	struct language_tls_dtor *item;

	/* This statically linked runtime does not support unloading DSOs. */
	ARG_UNUSED(dso_handle);

	item = malloc(sizeof(*item));
	if (item == NULL) {
		return -1;
	}

	*item = (struct language_tls_dtor){
		.function = function,
		.object = object,
		.next = language_tls_dtors,
	};
	language_tls_dtors = item;
	return 0;
}

static void *start_cpp_thread(void *argument)
{
	struct start_context context = *(struct start_context *)argument;
	void *result;

	free(argument);
	current_cpp_id = __real_pthread_self();
	result = context.entry(context.argument);
	run_language_tls_dtors();
	return result;
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*entry)(void *entry_argument), void *argument)
{
	struct start_context *context = malloc(sizeof(*context));
	int result;

	if (context == NULL) {
		return ENOMEM;
	}

	*context = (struct start_context){
		.entry = entry,
		.argument = argument,
	};
	result = __real_pthread_create(thread, attr, start_cpp_thread, context);
	if (result != 0) {
		free(context);
	}
	return result;
}

pthread_t __wrap_pthread_self(void)
{
	if (current_cpp_id == 0) {
		uint32_t id = (uint32_t)atomic_inc(&next_native_id) + 1U;

		/*
		 * Zephyr pthread handles have their high bit set.  Native
		 * callers get a disjoint, nonzero C++ identity.  It supports
		 * thread::id comparison, not pthread adoption or pthread TSS.
		 */
		if (id >= UINT32_C(0x80000000)) {
			k_panic();
		}
		current_cpp_id = (pthread_t)id;
	}
	return current_cpp_id;
}

FUNC_NORETURN void __wrap_pthread_exit(void *value)
{
	run_language_tls_dtors();
	__real_pthread_exit(value);
	CODE_UNREACHABLE;
}
