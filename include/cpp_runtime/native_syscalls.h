/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors */
#ifndef CPP_RUNTIME_NATIVE_SYSCALLS_H_
#define CPP_RUNTIME_NATIVE_SYSCALLS_H_

#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal adapter ABI. Successful calls return zero; errors are negative errno. */
enum cpp_runtime_sync_op {
	CPP_RUNTIME_MUTEX_INIT,
	CPP_RUNTIME_MUTEX_LOCK,
	CPP_RUNTIME_MUTEX_TRYLOCK,
	CPP_RUNTIME_MUTEX_UNLOCK,
	CPP_RUNTIME_MUTEX_DESTROY,
	CPP_RUNTIME_COND_SIGNAL,
	CPP_RUNTIME_COND_BROADCAST,
	CPP_RUNTIME_COND_WAIT,
	CPP_RUNTIME_COND_DESTROY,
};

typedef void *(*cpp_runtime_entry_t)(void *);

/* Fixed thread entry supplied by the user half; never invoked in a syscall. */
void cpp_runtime_thread_entry(void *entry, void *arg, void *unused);

/*
 * Handles live in application memory; kernel objects never do.
 * timeout_ticks is a relative timeout: -1 means forever, 0 means no wait.
 * Positive waits are bounded to one day's ticks; the adapter chunks longer waits.
 * other is the mutex handle for COND_WAIT and is otherwise ignored.
 */
__syscall int cpp_runtime_sync(uint64_t *handle, unsigned int op,
			      uint64_t *other, int64_t timeout_ticks);

__syscall int cpp_runtime_thread_create(k_tid_t *thread,
				       cpp_runtime_entry_t entry, void *arg);
__syscall int cpp_runtime_thread_join(k_tid_t thread);
__syscall int cpp_runtime_thread_detach(k_tid_t thread);

#ifdef __cplusplus
}
#endif

#include <zephyr/syscalls/native_syscalls.h>

#endif /* CPP_RUNTIME_NATIVE_SYSCALLS_H_ */
