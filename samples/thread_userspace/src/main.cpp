/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <cpp_runtime/thread.h>
#include <cpp_runtime/userspace.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/sys/printk.h>

K_THREAD_STACK_DEFINE(application_stack, 8192);
static struct k_thread application_thread;

struct shared_state {
	std::mutex mutex;
	std::condition_variable ready;
	bool complete = false;
	int answer = 0;
};

/* All C++ runtime use after activation takes place in this application domain.
 * Captures point into the shared application heap, never the parent's stack. */
static void application_body()
{
	auto state = std::make_shared<shared_state>();
	std::thread worker([state] {
		std::lock_guard<std::mutex> lock(state->mutex);

		state->answer = k_is_user_context() ? 42 : -1;
		state->complete = true;
		state->ready.notify_one();
	});

	{
		std::unique_lock<std::mutex> lock(state->mutex);

		state->ready.wait(lock, [state] { return state->complete; });
	}
	worker.join();
	printk("C++ user caller=%d worker result=%d\n", k_is_user_context(), state->answer);
}

static void application_entry(void *, void *, void *)
{
	application_body();
	/* All automatic objects are gone before language TLS and TSS cleanup. */
	cpp_runtime_thread_cleanup();
}

/* Privileged bootstrap uses C/Zephyr APIs only. It never consumes user heap
 * objects, C++ handler callbacks or runtime state once the user thread starts. */
int main()
{
	int ret = cpp_runtime_userspace_init(&k_mem_domain_default);

	if (ret != 0) {
		printk("runtime domain setup failed: %d\n", ret);
		return ret;
	}
#if Z_MALLOC_PARTITION_EXISTS
	ret = k_mem_domain_add_partition(&k_mem_domain_default, &z_malloc_partition);
	if (ret != 0) {
		printk("application heap setup failed: %d\n", ret);
		return ret;
	}
#endif
	k_tid_t child = k_thread_create(&application_thread, application_stack,
				       K_THREAD_STACK_SIZEOF(application_stack),
				       application_entry, nullptr, nullptr, nullptr,
				       5, K_USER | K_INHERIT_PERMS, K_NO_WAIT);

	return k_thread_join(child, K_FOREVER);
}
