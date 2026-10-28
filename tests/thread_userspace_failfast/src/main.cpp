/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "failfast_abort.h"

#include <cpp_runtime/native_syscalls.h>
#include <cpp_runtime/userspace.h>

#include <condition_variable>
#include <cstdlib>
#include <mutex>

#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/sys/printk.h>

BUILD_ASSERT(CPP_RUNTIME_FAILURE_CASE >= 1 && CPP_RUNTIME_FAILURE_CASE <= 3);

K_THREAD_STACK_DEFINE(application_stack, 8192);
static struct k_thread application_thread;

/*
 * Compiler TLS needs no extra MPU partition. The fatal handler reads only the
 * victim's own trivial TLS after checking the protected thread identity.
 */
static thread_local volatile unsigned int armed_case;
static thread_local volatile unsigned int resource_failure_proven;
static thread_local volatile unsigned int abort_seen;
static thread_local volatile unsigned int user_entry_seen;
#if CPP_RUNTIME_FAILURE_CASE == 3
static thread_local void *volatile allocation_observer;
#endif

static FUNC_NORETURN void fail(const char *message)
{
	armed_case = 0;
	abort_seen = 0;
	printk("FAILFAST FAIL case=%d: %s\n", CPP_RUNTIME_FAILURE_CASE, message);
	printk("PROJECT EXECUTION FAILED\n");
	k_oops();
	CODE_UNREACHABLE;
}

/* Observation only: the real libc abort() and architecture fatal path run. */
extern "C" FUNC_NORETURN void __wrap_abort(void)
{
	if (armed_case == CPP_RUNTIME_FAILURE_CASE && resource_failure_proven == 1U &&
	    user_entry_seen == 1U && k_is_user_context()) {
		abort_seen = CPP_RUNTIME_FAILURE_CASE;
		printk("FAILFAST observed abort case=%d user=1\n", CPP_RUNTIME_FAILURE_CASE);
	}
	__real_abort();
	CODE_UNREACHABLE;
}

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (k_current_get() == &application_thread && reason == K_ERR_KERNEL_OOPS &&
	    armed_case == CPP_RUNTIME_FAILURE_CASE &&
	    abort_seen == CPP_RUNTIME_FAILURE_CASE && resource_failure_proven == 1U &&
	    user_entry_seen == 1U) {
		printk("FAILFAST PASS case=%d user=1 abort=1 reason=%u\n",
		       CPP_RUNTIME_FAILURE_CASE, reason);
	} else {
		printk("FAILFAST FAIL case=%d unexpected fatal reason=%u victim=%p\n",
		       CPP_RUNTIME_FAILURE_CASE, reason, k_current_get());
		printk("PROJECT EXECUTION FAILED\n");
	}
	/* Each image has exactly one expected death. Never resume this domain. */
	k_fatal_halt(reason);
}

#if CPP_RUNTIME_FAILURE_CASE == 1
static void run_failure_case()
{
	uint64_t handles[CONFIG_CPP_RUNTIME_MUTEX_MAX + 1] = {};
	bool exhausted = false;

	for (unsigned int i = 0; i < ARRAY_SIZE(handles); ++i) {
		int ret = cpp_runtime_sync(&handles[i], CPP_RUNTIME_MUTEX_INIT, nullptr, 0);

		if (ret == -ENOMEM && i > 0U) {
			exhausted = true;
			break;
		}
		if (ret != 0) {
			fail("mutex pool setup returned an unexpected error");
		}
	}
	if (!exhausted) {
		fail("mutex pool exhaustion was not observed");
	}

	std::once_flag flag;

	resource_failure_proven = 1;
	armed_case = CPP_RUNTIME_FAILURE_CASE;
	/*
	 * This is the first call_once in a fresh image. Its internal mutex lock
	 * return is ignored by upstream libc++; the adapter must stop execution.
	 */
	std::call_once(flag, [] { fail("call_once ran without its required mutex"); });
	fail("call_once returned after mutex allocation failure");
}
#elif CPP_RUNTIME_FAILURE_CASE == 2
static void run_failure_case()
{
	uint64_t handles[CONFIG_CPP_RUNTIME_CONDVAR_MAX + 1] = {};
	bool exhausted = false;

	for (unsigned int i = 0; i < ARRAY_SIZE(handles); ++i) {
		int ret = cpp_runtime_sync(&handles[i], CPP_RUNTIME_COND_SIGNAL, nullptr, 0);

		if (ret == -ENOMEM && i > 0U) {
			exhausted = true;
			break;
		}
		if (ret != 0) {
			fail("condition-variable pool setup returned an unexpected error");
		}
	}
	if (!exhausted) {
		fail("condition-variable pool exhaustion was not observed");
	}

	std::condition_variable condition;

	resource_failure_proven = 1;
	armed_case = CPP_RUNTIME_FAILURE_CASE;
	/* notify_one is noexcept and ignores the backend's return value. */
	condition.notify_one();
	fail("notify_one returned after condition-variable allocation failure");
}
#else
struct destructor_probe {
	volatile unsigned int value = 0;

	~destructor_probe()
	{
		printk("FAILFAST unexpected TLS destruction\n");
	}
};

static void run_failure_case()
{
	unsigned int allocations = 0;

	/*
	 * This image deliberately never recovers. Retain every allocation and
	 * exhaust even the smallest allocatable block before TLS registration.
	 */
	for (;;) {
		void *block = std::malloc(1);

		allocation_observer = block;
		if (block == nullptr) {
			break;
		}
		if (++allocations > CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE) {
			fail("malloc exhaustion loop exceeded its bounded arena");
		}
	}
	if (allocations == 0U) {
		fail("malloc was unavailable before the injection");
	}
	printk("FAILFAST heap exhausted after %u allocations\n", allocations);
	resource_failure_proven = 1;
	armed_case = CPP_RUNTIME_FAILURE_CASE;

	/* The constructor itself does not allocate; registration needs a node. */
	thread_local destructor_probe probe;

	probe.value = 42;
	fail("TLS initialization returned after destructor registration OOM");
}
#endif

static void application_entry(void *, void *, void *)
{
	if (!k_is_user_context()) {
		fail("application entry has supervisor privilege");
	}
	user_entry_seen = 1;
	run_failure_case();
	fail("target operation unexpectedly returned");
}

/* Privileged bootstrap uses only C/Zephyr APIs and keeps the fatal identity. */
int main()
{
	int ret = cpp_runtime_userspace_init(&k_mem_domain_default);

	if (ret != 0) {
		printk("FAILFAST FAIL domain setup: %d\nPROJECT EXECUTION FAILED\n", ret);
		return ret;
	}
#if Z_MALLOC_PARTITION_EXISTS
	ret = k_mem_domain_add_partition(&k_mem_domain_default, &z_malloc_partition);
	if (ret != 0) {
		printk("FAILFAST FAIL heap setup: %d\nPROJECT EXECUTION FAILED\n", ret);
		return ret;
	}
#endif
	k_tid_t child = k_thread_create(&application_thread, application_stack,
				       K_THREAD_STACK_SIZEOF(application_stack),
				       application_entry, nullptr, nullptr, nullptr,
				       5, K_USER | K_INHERIT_PERMS, K_NO_WAIT);

	ret = k_thread_join(child, K_FOREVER);
	printk("FAILFAST FAIL user thread returned: %d\nPROJECT EXECUTION FAILED\n", ret);
	return -1;
}
