/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Thread entry
 *
 * This file provides the common thread entry function
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cfi.h>

#ifdef CONFIG_CURRENT_THREAD_USE_TLS
Z_THREAD_LOCAL k_tid_t z_tls_current;
#endif

#ifdef CONFIG_STACK_CANARIES_TLS
#include <zephyr/random/random.h>
extern Z_THREAD_LOCAL volatile uintptr_t __stack_chk_guard;
#endif /* CONFIG_STACK_CANARIES_TLS */

#if defined(CONFIG_THREAD_RESULT) || defined(CONFIG_THREAD_SPECIFIC_STORAGE)
FUNC_NORETURN void k_thread_exit(void *result)
{
#ifdef CONFIG_THREAD_RESULT
	k_thread_result_set(result);
#else
	ARG_UNUSED(result);
#endif

#ifdef CONFIG_THREAD_SPECIFIC_STORAGE
	/* A destructor may store another value. Visit only the values present at each pass. */
	for (unsigned int pass = 0; pass < CONFIG_THREAD_SPECIFIC_STORAGE_DESTRUCTOR_ITERATIONS;
	     pass++) {
		size_t count = k_thread_specific_count();
		void *key, *value;
		k_thread_key_destructor_t destructor;

		if (count == 0) {
			break;
		}

		while (count-- > 0 && k_thread_popspecific(&key, &value, &destructor) == 0) {
			if (destructor != NULL && value != NULL) {
				destructor(value);
			}
		}
	}

	/* Drop values stored again after the last pass. */
	void *key, *value;
	k_thread_key_destructor_t destructor;

	while (k_thread_popspecific(&key, &value, &destructor) == 0) {
	}
#endif

	k_thread_abort(k_current_get());
	CODE_UNREACHABLE;
}
#endif

/*
 * Common thread entry point function (used by all threads)
 *
 * This routine invokes the actual thread entry point function and passes
 * it three arguments. It also handles graceful termination of the thread
 * if the entry point function ever returns.
 *
 * This routine does not return, and is marked as such so the compiler won't
 * generate preamble code that is only used by functions that actually return.
 */
FUNC_NORETURN void z_thread_entry(k_thread_entry_t entry,
				 void *p1, void *p2, void *p3)
{
/*
 * Inform the unwinder that the current return address is undefined.
 *
 * This is typically used at points in the code where execution does not
 * return like a normal function (for example, thread entry routines).
 * Marking the return address as undefined prevents stack unwinding or
 * backtrace tools from following a bogus return address, which could
 * otherwise lead to reading invalid memory and cause faults during
 * unwinding or debugging.
 */
	ARCH_CFI_UNDEFINED_RETURN_ADDRESS();
#ifdef CONFIG_CURRENT_THREAD_USE_TLS
	z_tls_current = k_sched_current_thread_query();
#endif
#ifdef CONFIG_STACK_CANARIES_TLS
	uintptr_t stack_guard;

	sys_rand_get((uint8_t *)&stack_guard, sizeof(stack_guard));
	__stack_chk_guard = stack_guard;
	__stack_chk_guard <<= 8;
#endif	/* CONFIG_STACK_CANARIES_TLS */
	entry(p1, p2, p3);

#if defined(CONFIG_THREAD_RESULT)
	k_thread_exit(k_thread_result_get());
#elif defined(CONFIG_THREAD_SPECIFIC_STORAGE)
	k_thread_exit(NULL);
#else
	k_thread_abort(k_current_get());
#endif

	/*
	 * Compiler can't tell that k_thread_abort() won't return and issues a
	 * warning unless we tell it that control never gets this far.
	 */

	CODE_UNREACHABLE; /* LCOV_EXCL_LINE */
}
