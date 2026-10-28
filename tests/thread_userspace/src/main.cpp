/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <cpp_runtime/userspace.h>
#include <cpp_runtime/native_syscalls.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/libc-hooks.h>
#include <user_test_helpers.h>

using namespace std::chrono_literals;

BUILD_ASSERT(CONFIG_USERSPACE);
BUILD_ASSERT(__cplusplus == 201703L);
BUILD_ASSERT(CONFIG_CPP_RUNTIME_THREAD_MAX >= 4);

uint32_t user_static_value();
uint32_t supervisor_static_value();
uint32_t user_static_constructions();
uint32_t supervisor_static_constructions();

namespace {

constexpr auto wait_limit = 3s;
constexpr uint32_t worker_count = 4;
constexpr uint32_t iteration_count = 100;
constexpr uint32_t reuse_count = 4 * CONFIG_CPP_RUNTIME_THREAD_MAX;

/* A child must inherit the application's memory domain, in addition to being
 * unprivileged. Heap objects below reside in the libc malloc partition.
 * A user thread's stack is deliberately never shared with another thread.
 */
ZTEST_BMEM uint32_t application_marker;
ZTEST_BMEM int denied_domain_result;
ZTEST_BMEM bool denied_domain_user;

struct k_mem_domain denied_domain;
struct k_thread denied_thread;
alignas(uint64_t) uint64_t kernel_only_handle;
K_THREAD_STACK_DEFINE(denied_stack, 2048);

void denied_domain_entry(void *, void *, void *)
{
	uint64_t handle = 0;

	denied_domain_user = k_is_user_context();
	denied_domain_result = cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_INIT, nullptr, 0);
}

struct gate {
	std::mutex mutex;
	std::condition_variable cv;
	bool open = false;
	uint32_t waiting = 0;

	bool wait()
	{
		std::unique_lock<std::mutex> lock(mutex);

		++waiting;
		cv.notify_all();
		return cv.wait_for(lock, wait_limit, [this] { return open; });
	}

	bool release(uint32_t expected)
	{
		std::unique_lock<std::mutex> lock(mutex);
		const bool ready = cv.wait_for(lock, wait_limit,
					      [this, expected] { return waiting == expected; });

		open = true;
		lock.unlock();
		cv.notify_all();
		return ready;
	}
};

struct mutex_state {
	gate start;
	std::mutex mutex;
	uint32_t count = 0;
	std::atomic<uint32_t> failures{0};
};

struct tls_state {
	std::atomic<uint32_t> next{0};
	std::atomic<uint32_t> failures{0};
	std::atomic<uint32_t> completed{0};
};

struct tls_item {
	uint32_t expected;
	tls_state *state = nullptr;

	~tls_item()
	{
		if (state != nullptr) {
			if (!k_is_user_context() || state->next.fetch_add(1) != expected) {
				state->failures.fetch_add(1);
			}
			state->completed.fetch_add(1);
		}
	}
};

thread_local tls_item first_tls{1};
thread_local tls_item second_tls{0};

void touch_tls(tls_state *state)
{
	/* Reverse construction order must be used at thread exit. */
	first_tls.state = state;
	second_tls.state = state;
}

struct raw_thread_state {
	std::atomic<uint32_t> entered{0};
	std::atomic<uint32_t> exited{0};
	std::atomic<uint32_t> failures{0};
	std::atomic<bool> release{false};
};

void *raw_thread_entry(void *argument)
{
	auto *state = static_cast<raw_thread_state *>(argument);

	if (!k_is_user_context() || application_marker != 0x17c0ffeeU) {
		state->failures.fetch_add(1);
	}
	state->entered.fetch_add(1);
	while (!state->release.load()) {
		std::this_thread::sleep_for(1ms);
	}
	state->exited.fetch_add(1);
	return nullptr;
}

bool await_count(const std::atomic<uint32_t> &value, uint32_t expected)
{
	const auto limit = std::chrono::steady_clock::now() + wait_limit;

	while (value.load() != expected && std::chrono::steady_clock::now() < limit) {
		std::this_thread::sleep_for(1ms);
	}
	return value.load() == expected;
}

void *setup()
{
	zassert_equal(cpp_runtime_userspace_init(&k_mem_domain_default), 0,
		      "Runtime user-domain initialization failed");
	/* ARM's user TLS pointer belongs to the libc partition even for a
	 * probe that calls no libc/C++ functions. Runtime and malloc remain
	 * deliberately absent.
	 */
	struct k_mem_partition *partitions[] = {
#if Z_LIBC_PARTITION_EXISTS
		&z_libc_partition,
#endif
		&ztest_mem_partition,
	};

	zassert_equal(k_mem_domain_init(&denied_domain, ARRAY_SIZE(partitions), partitions), 0,
		      "Restricted domain initialization failed");
	/* This supervisor initialization runs before any user code is started.
	 * Later privileged hooks use only C/Zephyr APIs.
	 */
	zassert_equal(supervisor_static_value(), 42, "Supervisor initialization failed");
	return nullptr;
}

void before(void *)
{
	application_marker = 0x17c0ffeeU;
}

} /* namespace */

ZTEST_USER(libcxx_userspace, test_user_mode_and_domain_inheritance)
{
	zassert_true(k_is_user_context(), "Test is not running in user mode");

	struct state {
		bool child_user = false;
		bool grandchild_user = false;
		uint32_t child_marker = 0;
		uint32_t grandchild_marker = 0;
		std::thread::id child_id;
	};
	auto shared = std::make_shared<state>();
	std::thread worker([shared] {
		shared->child_user = k_is_user_context();
		shared->child_marker = application_marker;
		shared->child_id = std::this_thread::get_id();

		std::thread nested([shared] {
			shared->grandchild_user = k_is_user_context();
			shared->grandchild_marker = application_marker;
		});

		nested.join();
	});
	const auto id = worker.get_id();

	worker.join();

	zassert_true(shared->child_user, "std::thread elevated its caller's privilege");
	zassert_true(shared->grandchild_user, "Nested thread elevated its caller's privilege");
	zassert_equal(shared->child_marker, application_marker, "Child lost memory domain");
	zassert_equal(shared->grandchild_marker, application_marker,
		      "Nested child lost memory domain");
	zassert_true(id == shared->child_id, "Child ID differs from its owner handle");
	zassert_false(worker.joinable(), "Join did not release thread ownership");
}

ZTEST_USER(libcxx_userspace, test_user_child_cannot_raise_priority)
{
	struct state {
		int child_priority = -1;
		bool child_user = false;
	};
	auto shared = std::make_shared<state>();
	const int lowered_priority = CONFIG_CPP_RUNTIME_THREAD_PRIORITY + 2;

	k_thread_priority_set(k_current_get(), lowered_priority);
	std::thread worker([shared] {
		shared->child_priority = k_thread_priority_get(k_current_get());
		shared->child_user = k_is_user_context();
	});

	worker.join();
	zassert_true(shared->child_user, "Low-priority caller created a privileged child");
	zassert_true(shared->child_priority >= lowered_priority,
		     "User caller created a higher-priority child");
	/* The test thread exits next. Raising its own priority back would itself
	 * violate the userspace priority policy.
	 */
}

ZTEST_USER(libcxx_userspace, test_static_local_initialization)
{
	struct state {
		gate start;
		std::atomic<uint32_t> failures{0};
	};
	auto shared = std::make_shared<state>();
	std::array<std::thread, worker_count> workers;

	for (auto &worker : workers) {
		worker = std::thread([shared] {
			if (!shared->start.wait() || user_static_value() != 42 ||
			    supervisor_static_value() != 42) {
				shared->failures.fetch_add(1);
			}
		});
	}
	const bool ready = shared->start.release(worker_count);

	for (auto &worker : workers) {
		worker.join();
	}
	zassert_true(ready, "Static-initialization workers did not reach the gate");
	zassert_equal(shared->failures.load(), 0, "Static value was not published");
	zassert_equal(user_static_constructions(), 1, "User static constructed repeatedly");
	zassert_equal(supervisor_static_constructions(), 1,
		      "Supervisor static constructed repeatedly");
}

ZTEST_USER(libcxx_userspace, test_mutex_created_by_sibling)
{
	auto shared = std::make_shared<mutex_state>();
	std::array<std::thread, worker_count> workers;

	/* Neither mutex is initialized before the children are created. The
	 * first child initializes it; every sibling must be able to use it.
	 */
	for (auto &worker : workers) {
		worker = std::thread([shared] {
			if (!k_is_user_context() || !shared->start.wait()) {
				shared->failures.fetch_add(1);
				return;
			}
			for (uint32_t i = 0; i < iteration_count; ++i) {
				std::lock_guard<std::mutex> lock(shared->mutex);

				++shared->count;
				std::this_thread::yield();
			}
		});
	}
	const bool ready = shared->start.release(worker_count);

	for (auto &worker : workers) {
		worker.join();
	}
	zassert_true(ready, "Workers did not reach the gate");
	zassert_equal(shared->failures.load(), 0, "User mode or gate check failed");
	zassert_equal(shared->count, worker_count * iteration_count, "Lost protected updates");
}

ZTEST_USER(libcxx_userspace, test_recursive_and_timed_mutex)
{
	struct state {
		std::recursive_timed_mutex recursive;
		std::timed_mutex timed;
		bool recursive_acquired = false;
		bool timed_acquired = false;
	};
	auto shared = std::make_shared<state>();

	shared->recursive.lock();
	shared->recursive.lock();
	shared->timed.lock();
	std::thread blocked([shared] {
		shared->recursive_acquired = shared->recursive.try_lock_for(20ms);
		if (shared->recursive_acquired) {
			shared->recursive.unlock();
		}
		shared->timed_acquired = shared->timed.try_lock_until(
			std::chrono::steady_clock::now() + 20ms);
		if (shared->timed_acquired) {
			shared->timed.unlock();
		}
	});

	blocked.join();
	zassert_false(shared->recursive_acquired, "Recursive lock admitted another owner");
	zassert_false(shared->timed_acquired, "Timed lock admitted another owner");
	shared->recursive.unlock();
	shared->recursive.unlock();
	shared->timed.unlock();

	std::thread available([shared] {
		shared->recursive_acquired = shared->recursive.try_lock_for(wait_limit);
		if (shared->recursive_acquired) {
			shared->recursive.unlock();
		}
		shared->timed_acquired = shared->timed.try_lock_for(wait_limit);
		if (shared->timed_acquired) {
			shared->timed.unlock();
		}
	});

	available.join();
	zassert_true(shared->recursive_acquired, "Recursive lock remained unavailable");
	zassert_true(shared->timed_acquired, "Timed lock remained unavailable");
}

ZTEST_USER(libcxx_userspace, test_condition_variable_timeout_and_notification)
{
	struct state {
		std::mutex mutex;
		std::condition_variable cv;
		bool ready = false;
	};
	auto shared = std::make_shared<state>();
	std::unique_lock<std::mutex> lock(shared->mutex);
	const auto started = std::chrono::steady_clock::now();
	const bool timed_out = !shared->cv.wait_for(lock, 20ms, [] { return false; });
	const auto elapsed = std::chrono::steady_clock::now() - started;
	std::thread worker([shared] {
		std::lock_guard<std::mutex> guard(shared->mutex);

		shared->ready = true;
		shared->cv.notify_one();
	});
	const bool woke = shared->cv.wait_for(lock, wait_limit,
					     [shared] { return shared->ready; });

	lock.unlock();
	worker.join();
	zassert_true(timed_out, "False predicate unexpectedly succeeded");
	zassert_true(elapsed >= 20ms, "Timed wait returned early");
	zassert_true(woke, "Condition notification was lost");
}

ZTEST_USER(libcxx_userspace, test_shared_mutex)
{
	struct state {
		std::shared_timed_mutex mutex;
		bool reader = false;
		bool writer = false;
	};
	auto shared = std::make_shared<state>();

	shared->mutex.lock_shared();
	std::thread worker([shared] {
		shared->reader = shared->mutex.try_lock_shared();
		if (shared->reader) {
			shared->mutex.unlock_shared();
		}
		shared->writer = shared->mutex.try_lock_for(20ms);
		if (shared->writer) {
			shared->mutex.unlock();
		}
	});

	worker.join();
	shared->mutex.unlock_shared();
	zassert_true(shared->reader, "Concurrent readers were rejected");
	zassert_false(shared->writer, "Writer acquired with a reader present");
}

ZTEST_USER(libcxx_userspace, test_synchronization_storage_reuse)
{
	struct state {
		std::shared_mutex shared;
		std::mutex mutex;
		std::condition_variable cv;
		bool ready = false;
		bool reader = false;
	};
	/* Reconstructing at exactly the same address must discard old kernel
	 * handles. Allocator reuse alone is not a deterministic check of this.
	 */
	void *storage = ::operator new(sizeof(state));

	for (uint32_t i = 0; i < reuse_count; ++i) {
		auto *shared = new (storage) state;
		std::unique_lock<std::mutex> lock(shared->mutex);

		shared->shared.lock_shared();
		std::thread worker([shared] {
			shared->reader = shared->shared.try_lock_shared();
			if (shared->reader) {
				shared->shared.unlock_shared();
			}
			std::lock_guard<std::mutex> guard(shared->mutex);

			shared->ready = true;
			shared->cv.notify_one();
		});
		const bool ready = shared->cv.wait_for(lock, wait_limit,
						      [shared] { return shared->ready; });

		lock.unlock();
		worker.join();
		shared->shared.unlock_shared();
		const bool reader = shared->reader;

		shared->~state();
		zassert_true(ready, "Reconstructed condition variable lost notification");
		zassert_true(reader, "Reconstructed shared mutex rejected a reader");
	}
	::operator delete(storage);
}

ZTEST_USER(libcxx_userspace, test_call_once)
{
	struct state {
		gate start;
		std::once_flag once;
		std::atomic<uint32_t> initialized{0};
		std::atomic<uint32_t> failures{0};
		uint32_t value = 0;
	};
	auto shared = std::make_shared<state>();
	std::array<std::thread, worker_count> workers;

	for (auto &worker : workers) {
		worker = std::thread([shared] {
			if (!shared->start.wait()) {
				shared->failures.fetch_add(1);
				return;
			}
			std::call_once(shared->once, [shared] {
				shared->initialized.fetch_add(1);
				std::this_thread::sleep_for(10ms);
				shared->value = 42;
			});
			if (shared->value != 42) {
				shared->failures.fetch_add(1);
			}
		});
	}
	const bool ready = shared->start.release(worker_count);

	for (auto &worker : workers) {
		worker.join();
	}
	zassert_true(ready, "Workers did not reach the gate");
	zassert_equal(shared->initialized.load(), 1, "call_once ran more than once");
	zassert_equal(shared->failures.load(), 0, "call_once failed to publish state");
}

ZTEST_USER(libcxx_userspace, test_future_and_async)
{
	std::promise<uint32_t> promise;
	auto future = promise.get_future();
	std::thread worker([promise = std::move(promise)]() mutable {
		promise.set_value(k_is_user_context() ? 42U : 0U);
	});
	const auto ready = future.wait_for(wait_limit);

	worker.join();
	zassert_true(ready == std::future_status::ready, "Promise did not complete");
	zassert_equal(future.get(), 42, "Promise worker was privileged or result changed");

	auto async = std::async(std::launch::async, [] {
		return k_is_user_context() ? 17U : 0U;
	});

	zassert_true(async.wait_for(wait_limit) == std::future_status::ready,
		     "Async did not complete");
	zassert_equal(async.get(), 17, "Async worker was privileged");
}

ZTEST_USER(libcxx_userspace, test_sleep_survives_early_wakeup)
{
	struct state {
		std::atomic<bool> started{false};
		std::atomic<bool> slept{false};
		std::atomic<bool> allow_exit{false};
		int64_t elapsed_ns = 0;
	};
	auto shared = std::make_shared<state>();
	std::thread worker([shared] {
		const auto started = std::chrono::steady_clock::now();

		shared->started.store(true);
		std::this_thread::sleep_for(100ms);
		shared->elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - started).count();
		shared->slept.store(true);
		/* Keep the native object initialized until the creator has finished
		 * issuing wakeups. An exited Zephyr thread rejects k_wakeup().
		 */
		while (!shared->allow_exit.load()) {
			std::this_thread::sleep_for(1ms);
		}
	});
	const auto limit = std::chrono::steady_clock::now() + wait_limit;

	while (!shared->started.load() && std::chrono::steady_clock::now() < limit) {
		std::this_thread::sleep_for(1ms);
	}
	const bool started = shared->started.load();
	unsigned int early_wakeups = 0;

	/* The creator has native-handle permission, but a kernel wakeup may
	 * interrupt only the current wait, not shorten C++ sleep_for overall.
	 */
	for (unsigned int i = 0; i < 3; ++i) {
		std::this_thread::sleep_for(3ms);
		early_wakeups += !shared->slept.load();
		k_wakeup(worker.native_handle());
	}
	shared->allow_exit.store(true);
	worker.join();
	zassert_true(started, "Sleeping child did not start");
	zassert_true(early_wakeups > 0, "Test did not wake the child before sleep completion");
	zassert_true(shared->elapsed_ns >= 100000000, "Native wakeup shortened sleep_for");
}

ZTEST_USER(libcxx_userspace, test_sleep_fractional_tick_early_wakeup)
{
	struct state {
		std::atomic<bool> armed{false};
		std::atomic<bool> finished{false};
		std::atomic<bool> allow_exit{false};
		bool aligned = false;
		int64_t wake_tick = 0;
		int64_t requested_ns = 0;
		uint64_t phase_cycles = 0;
		uint32_t started_cycles = 0;
		uint32_t elapsed_cycles = 0;
	};
	const uint64_t tick_cycles = k_ticks_to_cyc_ceil64(1);
	const int64_t requested_ns = k_ticks_to_ns_ceil64(1);
	const uint64_t required_cycles = k_ns_to_cyc_ceil64(requested_ns);
	bool exercised_early_wakeup = false;

	zassert_true(tick_cycles > 4 && required_cycles < UINT32_MAX / 4,
		     "Test needs a measurable tick shorter than counter wrap");
	for (unsigned int attempt = 0; attempt < 32 && !exercised_early_wakeup; ++attempt) {
		auto shared = std::make_shared<state>();

		shared->requested_ns = requested_ns;
		shared->phase_cycles = tick_cycles / 2U;
		std::thread worker([shared] {
			/* Align using the hardware cycle clock, because chrono's
			 * tick-resolution clock would conceal a fractional early return.
			 */
			for (unsigned int alignment = 0; alignment < 8; ++alignment) {
				const int64_t previous = k_uptime_ticks();
				int64_t boundary;

				do {
					boundary = k_uptime_ticks();
				} while (boundary == previous);
				const uint32_t cycle_boundary = cpp_test_cycle_get32();

				while (static_cast<uint32_t>(cpp_test_cycle_get32() - cycle_boundary) <
				       shared->phase_cycles) {
					/* The user-safe cycle syscall performs no raw timer I/O. */
				}
				if (k_uptime_ticks() != boundary) {
					continue;
				}
				shared->aligned = true;
				shared->wake_tick = boundary + 1;
				shared->started_cycles = cpp_test_cycle_get32();
				shared->armed.store(true);
				std::this_thread::sleep_for(std::chrono::nanoseconds(shared->requested_ns));
				shared->elapsed_cycles = cpp_test_cycle_get32() - shared->started_cycles;
				break;
			}
			shared->finished.store(true);
			while (!shared->allow_exit.load()) {
				std::this_thread::yield();
			}
		});
		const auto limit = std::chrono::steady_clock::now() + wait_limit;

		while (!shared->armed.load() && !shared->finished.load() &&
		       std::chrono::steady_clock::now() < limit) {
			std::this_thread::yield();
		}
		if (shared->armed.load()) {
			(void)k_sleep(K_TIMEOUT_ABS_TICKS(shared->wake_tick));
			const uint32_t elapsed = cpp_test_cycle_get32() - shared->started_cycles;

			exercised_early_wakeup = elapsed < required_cycles;
			k_wakeup(worker.native_handle());
		}
		shared->allow_exit.store(true);
		worker.join();
		if (!shared->aligned) {
			/* Host-scheduled SMP emulation can miss the injection window.
			 * Retry, but never count an unexercised window as a pass.
			 */
			continue;
		}
		zassert_true(shared->elapsed_cycles >= required_cycles,
			     "Early wakeup rounded a fractional tick into a full sleep");
	}
	zassert_true(exercised_early_wakeup, "Scheduler never exercised the early-wakeup window");
}

ZTEST_USER(libcxx_userspace, test_tls_destructors_before_join)
{
	auto shared = std::make_shared<tls_state>();
	std::thread worker([shared] { touch_tls(shared.get()); });

	worker.join();
	zassert_equal(shared->completed.load(), 2, "Join preceded TLS destructor completion");
	zassert_equal(shared->failures.load(), 0, "TLS order or privilege changed");
}

ZTEST_USER(libcxx_userspace, test_notify_at_exit_follows_tls_destructors)
{
	struct state {
		std::mutex mutex;
		std::condition_variable cv;
		tls_state tls;
		bool ready = false;
	};
	auto shared = std::make_shared<state>();
	std::unique_lock<std::mutex> lock(shared->mutex);
	std::thread worker([shared] {
		touch_tls(&shared->tls);
		std::unique_lock<std::mutex> guard(shared->mutex);

		shared->ready = true;
		std::notify_all_at_thread_exit(shared->cv, std::move(guard));
	});
	const bool ready = shared->cv.wait_for(lock, wait_limit,
					      [shared] { return shared->ready; });
	const auto completed = shared->tls.completed.load();

	lock.unlock();
	worker.join();
	zassert_true(ready, "Thread-exit notification was lost");
	zassert_equal(completed, 2, "Notification preceded TLS destructors");
	zassert_equal(shared->tls.failures.load(), 0, "TLS destructor ordering failed");
}

ZTEST_USER(libcxx_userspace, test_join_and_detach_reuse)
{
	for (uint32_t i = 0; i < reuse_count; ++i) {
		auto promise = std::make_shared<std::promise<bool>>();
		auto done = promise->get_future();
		std::thread worker([promise] {
			promise->set_value_at_thread_exit(k_is_user_context());
		});

		if ((i & 1U) == 0) {
			worker.detach();
		} else {
			worker.join();
		}
		zassert_false(worker.joinable(), "Thread handle retains ownership");
		zassert_true(done.wait_for(wait_limit) == std::future_status::ready,
			     "Reused thread did not complete");
		zassert_true(done.get(), "Reused thread was privileged");
	}
}

ZTEST_USER(libcxx_userspace, test_thread_pool_exhaustion_and_recovery)
{
	auto shared = std::make_shared<raw_thread_state>();
	std::array<std::__libcpp_thread_t, CONFIG_CPP_RUNTIME_THREAD_MAX> handles{};
	uint32_t created = 0;
	int create_error = 0;

	/* The lower-level adapter returns errors even when C++ exceptions are
	 * disabled. Constructing a failing std::thread would terminate instead.
	 */
	for (auto &handle : handles) {
		const auto limit = std::chrono::steady_clock::now() + wait_limit;

		do {
			create_error = std::__libcpp_thread_create(&handle, raw_thread_entry,
								  shared.get());
			if (create_error == EAGAIN) {
				std::this_thread::sleep_for(1ms);
			}
		} while (create_error == EAGAIN && std::chrono::steady_clock::now() < limit);
		if (create_error != 0) {
			break;
		}
		++created;
	}
	std::__libcpp_thread_t extra{};
	const int exhausted = std::__libcpp_thread_create(&extra, raw_thread_entry, shared.get());
	const bool all_started = await_count(shared->entered, created);

	shared->release.store(true);
	int join_errors = 0;

	for (uint32_t i = 0; i < created; ++i) {
		join_errors += std::__libcpp_thread_join(&handles[i]) != 0;
	}
	if (exhausted == 0) {
		join_errors += std::__libcpp_thread_join(&extra) != 0;
	}
	zassert_equal(create_error, 0, "Pool exhausted before configured capacity");
	zassert_equal(created, CONFIG_CPP_RUNTIME_THREAD_MAX, "Configured slots unavailable");
	zassert_equal(exhausted, EAGAIN, "Full thread pool did not return EAGAIN");
	zassert_true(all_started, "Allocated threads did not start");
	zassert_equal(join_errors, 0, "Pool exhaustion damaged existing threads");
	zassert_equal(shared->failures.load(), 0, "Pool worker lost userspace or domain");

	std::__libcpp_thread_t recovered{};
	const int recovery = std::__libcpp_thread_create(&recovered, raw_thread_entry, shared.get());

	if (recovery == 0) {
		zassert_equal(std::__libcpp_thread_join(&recovered), 0, "Recovery join failed");
	}
	zassert_equal(recovery, 0, "Released thread slots could not be reused");
}

ZTEST_USER(libcxx_userspace, test_forged_and_stale_sync_handles)
{
	uint64_t original = 0;

	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_INIT, nullptr, 0), 0,
		      "Mutex initialization failed");
	const uint64_t first_token = original;
	uint64_t copied = original;

	original = 0;
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_INIT, nullptr, 0), -EINVAL,
		      "Zeroing a live token allocated a second mutex");
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_COND_SIGNAL, nullptr, 0), -EINVAL,
		      "Changing type at a live handle address allocated another object");
	original = first_token;

	zassert_equal(cpp_runtime_sync(&copied, CPP_RUNTIME_MUTEX_LOCK, nullptr, 0), -EINVAL,
		      "A token copied to another object address was accepted");
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_DESTROY, nullptr, 0), 0,
		      "Mutex destruction failed");
	zassert_equal(original, 0, "Destruction did not clear the handle");
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_INIT, nullptr, 0), 0,
		      "Mutex reinitialization failed");
	const uint64_t current_token = original;

	original = first_token;
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_LOCK, nullptr, 0), -EINVAL,
		      "Stale generation was accepted at a reused object address");
	original = current_token;
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_LOCK, nullptr, 0), 0,
		      "Invalid request damaged the valid object");
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_UNLOCK, nullptr, 0), 0,
		      "Valid mutex unlock failed");
	zassert_equal(cpp_runtime_sync(&original, CPP_RUNTIME_MUTEX_DESTROY, nullptr, 0), 0,
		      "Valid mutex cleanup failed");
}

ZTEST_USER(libcxx_userspace, test_sync_pool_exhaustion_and_recovery)
{
	std::array<uint64_t, CONFIG_CPP_RUNTIME_MUTEX_MAX + 1> mutexes{};
	std::array<uint64_t, CONFIG_CPP_RUNTIME_CONDVAR_MAX + 1> condvars{};
	size_t mutex_count = 0;
	size_t condvar_count = 0;
	int mutex_error = 0;
	int condvar_error = 0;
	int cleanup_errors = 0;

	/* Existing libc++ globals may own some pool entries. Fill every
	 * remaining entry without calling any C++ operation that needs a lock.
	 */
	for (auto &handle : mutexes) {
		mutex_error = cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_INIT, nullptr, 0);
		if (mutex_error != 0) {
			break;
		}
		++mutex_count;
	}
	for (auto &handle : condvars) {
		condvar_error = cpp_runtime_sync(&handle, CPP_RUNTIME_COND_SIGNAL, nullptr, 0);
		if (condvar_error != 0) {
			break;
		}
		++condvar_count;
	}
	for (size_t i = 0; i < condvar_count; ++i) {
		cleanup_errors += cpp_runtime_sync(&condvars[i], CPP_RUNTIME_COND_DESTROY,
						   nullptr, 0) != 0;
	}
	for (size_t i = 0; i < mutex_count; ++i) {
		cleanup_errors += cpp_runtime_sync(&mutexes[i], CPP_RUNTIME_MUTEX_DESTROY,
						   nullptr, 0) != 0;
	}
	zassert_true(mutex_count > 0 && mutex_count <= CONFIG_CPP_RUNTIME_MUTEX_MAX,
		     "No mutex capacity or configured bound ignored");
	zassert_true(condvar_count > 0 && condvar_count <= CONFIG_CPP_RUNTIME_CONDVAR_MAX,
		     "No condition-variable capacity or configured bound ignored");
	zassert_equal(mutex_error, -ENOMEM, "Mutex exhaustion did not return ENOMEM");
	zassert_equal(condvar_error, -ENOMEM, "Condition exhaustion was confused with timeout");
	zassert_equal(cleanup_errors, 0, "Exhaustion damaged allocated objects");

	uint64_t mutex = 0;
	uint64_t condvar = 0;

	zassert_equal(cpp_runtime_sync(&mutex, CPP_RUNTIME_MUTEX_LOCK, nullptr, 0), 0,
		      "Mutex capacity did not recover");
	zassert_equal(cpp_runtime_sync(&condvar, CPP_RUNTIME_COND_WAIT, &mutex, 0), -EAGAIN,
		      "An actual condition timeout must retain its distinct errno");
	zassert_equal(cpp_runtime_sync(&mutex, CPP_RUNTIME_MUTEX_UNLOCK, nullptr, 0), 0,
		      "Condition timeout did not return with the mutex held");
	zassert_equal(cpp_runtime_sync(&condvar, CPP_RUNTIME_COND_DESTROY, nullptr, 0), 0,
		      "Recovered condition variable cleanup failed");
	zassert_equal(cpp_runtime_sync(&mutex, CPP_RUNTIME_MUTEX_DESTROY, nullptr, 0), 0,
		      "Recovered mutex cleanup failed");
}

ZTEST_USER(libcxx_userspace, test_bad_sync_arguments)
{
	uint64_t handle = 0;

	zassert_equal(cpp_runtime_userspace_init(&k_mem_domain_default), -EPERM,
		      "User caller was allowed to register a runtime memory domain");
	zassert_equal(cpp_runtime_sync(&handle, UINT32_MAX, nullptr, 0), -EINVAL,
		      "Invalid operation was accepted");
	zassert_equal(cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_INIT, nullptr, -2), -EINVAL,
		      "Invalid timeout was accepted");
	zassert_equal(cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_INIT, nullptr, INT64_MAX),
		      -EINVAL, "Overflowing timeout was accepted");
	zassert_equal(cpp_runtime_sync(nullptr, CPP_RUNTIME_MUTEX_INIT, nullptr, 0), -EINVAL,
		      "NULL handle was accepted");
	zassert_equal(cpp_runtime_sync(&kernel_only_handle,
				       CPP_RUNTIME_MUTEX_INIT, nullptr, 0), -EFAULT,
		      "Kernel-only memory was accepted as a writable handle");
	zassert_equal(handle, 0, "Invalid requests allocated a handle");
}

ZTEST_USER(libcxx_userspace, test_busy_mutex_cannot_be_destroyed)
{
	uint64_t handle = 0;

	zassert_equal(cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_LOCK, nullptr, 0), 0,
		      "Mutex lock failed");
	const uint64_t token = handle;

	zassert_equal(cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_DESTROY, nullptr, 0), -EBUSY,
		      "Locked mutex was destroyed");
	zassert_equal(handle, token, "Failed destruction changed the handle");
	zassert_equal(cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_UNLOCK, nullptr, 0), 0,
		      "Busy destruction damaged the mutex");
	zassert_equal(cpp_runtime_sync(&handle, CPP_RUNTIME_MUTEX_DESTROY, nullptr, 0), 0,
		      "Unlocked mutex could not be destroyed");
}

ZTEST_USER(libcxx_userspace, test_waiting_condvar_cannot_be_destroyed)
{
	struct state {
		std::mutex mutex;
		std::condition_variable cv;
		std::atomic<bool> entering{false};
		bool release = false;
		bool completed = false;
	};
	auto shared = std::make_shared<state>();
	std::thread worker([shared] {
		std::unique_lock<std::mutex> lock(shared->mutex);

		shared->entering.store(true);
		shared->completed = shared->cv.wait_for(lock, wait_limit,
						       [shared] { return shared->release; });
	});
	const auto limit = std::chrono::steady_clock::now() + wait_limit;

	while (!shared->entering.load() && std::chrono::steady_clock::now() < limit) {
		std::this_thread::sleep_for(1ms);
	}
	const bool entered = shared->entering.load();
	std::unique_lock<std::mutex> lock(shared->mutex);
	/* Acquiring the mutex after 'entering' proves the worker released it
	 * inside the atomic condition wait; the blocked operation owns a pin.
	 */
	const int destroyed = cpp_runtime_sync(&shared->cv.native_handle()->handle,
					       CPP_RUNTIME_COND_DESTROY, nullptr, 0);

	shared->release = true;
	lock.unlock();
	shared->cv.notify_one();
	worker.join();
	zassert_true(entered, "Worker did not reach the condition wait");
	zassert_equal(destroyed, -EBUSY, "A condition variable with a waiter was destroyed");
	zassert_true(shared->completed, "Rejected destruction damaged the waiter");
}

ZTEST(libcxx_userspace, test_unregistered_domain_is_denied)
{
	zassert_equal(cpp_runtime_userspace_init(&k_mem_domain_default), 0,
		      "Repeated domain registration was not idempotent");
	zassert_equal(cpp_runtime_userspace_init(&denied_domain), -EALREADY,
		      "A second C++ runtime domain was accepted");
	denied_domain_result = 0;
	denied_domain_user = false;
	k_thread_create(&denied_thread, denied_stack, K_THREAD_STACK_SIZEOF(denied_stack),
			denied_domain_entry, nullptr, nullptr, nullptr,
			CONFIG_CPP_RUNTIME_THREAD_PRIORITY, K_USER, K_FOREVER);
	zassert_equal(k_mem_domain_add_thread(&denied_domain, &denied_thread), 0,
		      "Unable to move thread into restricted domain");
	k_thread_start(&denied_thread);
	zassert_equal(k_thread_join(&denied_thread, K_SECONDS(3)), 0,
		      "Restricted-domain thread did not exit");
	zassert_true(denied_domain_user, "Restricted-domain probe was not unprivileged");
	zassert_equal(denied_domain_result, -EACCES,
		      "Unregistered domain gained access to runtime synchronization");
}

ZTEST_USER(libcxx_userspace, test_invalid_and_self_join)
{
	auto self = std::__libcpp_thread_get_current_id();

	zassert_equal(std::__libcpp_thread_join(&self), EDEADLK, "Self-join did not fail");
	std::__libcpp_thread_t invalid{};

	zassert_equal(std::__libcpp_thread_join(&invalid), EINVAL, "Empty join was accepted");
	zassert_equal(std::__libcpp_thread_detach(&invalid), EINVAL, "Empty detach was accepted");
}

ZTEST_SUITE(libcxx_userspace, nullptr, setup, before, nullptr, nullptr);
