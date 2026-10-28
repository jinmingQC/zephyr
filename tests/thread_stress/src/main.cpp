/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cpp_runtime/thread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <zephyr/ztest.h>

using namespace std::chrono_literals;

namespace {

constexpr uint32_t capacity = CONFIG_CPP_RUNTIME_THREAD_MAX;
constexpr uint32_t creator_count = 4;
constexpr uint32_t create_rounds = 96;

BUILD_ASSERT(capacity >= 2 * creator_count);

struct blocked_context {
	struct k_sem entered;
	struct k_sem release;
	std::atomic<uint32_t> failures{0};
	std::atomic<uint32_t> completed{0};
};

void prepare(blocked_context &context)
{
	k_sem_init(&context.entered, 0, capacity);
	k_sem_init(&context.release, 0, capacity);
}

void *blocked_entry(void *arg)
{
	auto &context = *static_cast<blocked_context *>(arg);

	k_sem_give(&context.entered);
	if (k_sem_take(&context.release, K_SECONDS(2)) != 0) {
		context.failures.fetch_add(1);
	}
	context.completed.fetch_add(1);
	return nullptr;
}

void *increment_entry(void *arg)
{
	static_cast<std::atomic<uint32_t> *>(arg)->fetch_add(1);
	return nullptr;
}

K_THREAD_STACK_DEFINE(native_root_stack, 4096);
struct k_thread native_root_thread;

std::__libcpp_tls_key order_key;
std::__libcpp_tls_key rearm_key;
std::array<uint32_t, 8> exit_order;
uint32_t exit_order_size;
std::atomic<uint32_t> destructor_failures{0};

void record_exit(uint32_t value)
{
	if (exit_order_size < exit_order.size()) {
		exit_order[exit_order_size++] = value;
	} else {
		destructor_failures.fetch_add(1);
	}
}

struct destructor_probe {
	uint32_t value;

	~destructor_probe();
};

destructor_probe &late_probe()
{
	thread_local destructor_probe probe{3};

	return probe;
}

destructor_probe::~destructor_probe()
{
	record_exit(value);
	if (value == 2) {
		/* Registration during a destructor must extend the same LIFO list. */
		(void)late_probe();
	}
}

void ordered_tss_destructor(void *arg)
{
	if (std::__libcpp_tls_get(order_key) != nullptr) {
		destructor_failures.fetch_add(1);
	}
	record_exit(*static_cast<uint32_t *>(arg));
}

struct rearm_context {
	uint32_t calls = 0;
	uint32_t failures = 0;
};

void rearming_tss_destructor(void *arg)
{
	auto &context = *static_cast<rearm_context *>(arg);

	++context.calls;
	if (std::__libcpp_tls_get(rearm_key) != nullptr ||
	    std::__libcpp_tls_set(rearm_key, arg) != 0) {
		++context.failures;
	}
}

void *ordered_tls_entry(void *arg)
{
	thread_local destructor_probe first{1};
	thread_local destructor_probe second{2};

	(void)first;
	(void)second;
	if (std::__libcpp_tls_get(order_key) != nullptr ||
	    std::__libcpp_tls_set(order_key, arg) != 0) {
		destructor_failures.fetch_add(1);
	}
	return nullptr;
}

void native_root_entry(void *arg, void *, void *)
{
	(void)ordered_tls_entry(arg);
	cpp_runtime_thread_cleanup();
}

void *rearming_tls_entry(void *arg)
{
	auto &context = *static_cast<rearm_context *>(arg);

	if (std::__libcpp_tls_get(rearm_key) != nullptr ||
	    std::__libcpp_tls_set(rearm_key, arg) != 0) {
		++context.failures;
	}
	return nullptr;
}

atomic_ptr_t sleep_wake_target;
atomic_t sleep_wake_cycle;
atomic_t sleep_wake_was_sleeping;
atomic_t sleep_wake_done;

void interrupt_sleep(struct k_timer *)
{
	k_tid_t target = static_cast<k_tid_t>(atomic_ptr_get(&sleep_wake_target));
	char state[64];

	atomic_set(&sleep_wake_was_sleeping,
		   std::strstr(k_thread_state_str(target, state, sizeof(state)), "sleeping") !=
			   nullptr ? 1 : 0);
	atomic_set(&sleep_wake_cycle, static_cast<atomic_val_t>(k_cycle_get_32()));
	k_wakeup(target);
	/* The test waits for this before reusing the callback's shared state. */
	atomic_set(&sleep_wake_done, 1);
}

K_TIMER_DEFINE(sleep_wake_timer, interrupt_sleep, nullptr);

void *setup()
{
	zassert_equal(std::__libcpp_tls_create(&order_key, ordered_tss_destructor), 0);
	zassert_equal(std::__libcpp_tls_create(&rearm_key, rearming_tss_destructor), 0);
	return nullptr;
}

} /* namespace */

ZTEST(libcxx_thread_stress, test_pool_capacity_and_join_ownership)
{
	blocked_context context;
	std::array<std::__libcpp_thread_t, capacity> handles{};
	std::atomic<uint32_t> replacement_runs{0};

	prepare(context);
	for (auto &handle : handles) {
		zassert_equal(std::__libcpp_thread_create(&handle, blocked_entry, &context), 0);
	}
	for (uint32_t i = 0; i < capacity; ++i) {
		zassert_equal(k_sem_take(&context.entered, K_SECONDS(2)), 0);
	}

	auto extra = k_current_get();
	const auto unchanged = extra;

	zassert_equal(std::__libcpp_thread_create(&extra, increment_entry, &replacement_runs),
		      EAGAIN, "A full pool admitted another thread");
	zassert_true(extra == unchanged, "Failed create overwrote its output handle");

	for (uint32_t i = 0; i < capacity; ++i) {
		k_sem_give(&context.release);
	}
	for (auto handle : handles) {
		zassert_equal(k_thread_join(handle, K_SECONDS(2)), 0);
	}
	zassert_equal(std::__libcpp_thread_create(&extra, increment_entry, &replacement_runs),
		      EAGAIN, "Terminated joinable threads lost their ownership slots");

	zassert_equal(std::__libcpp_thread_join(&handles[0]), 0);
	zassert_equal(std::__libcpp_thread_create(&extra, increment_entry, &replacement_runs), 0);
	zassert_equal(std::__libcpp_thread_join(&extra), 0);
	for (uint32_t i = 1; i < capacity; ++i) {
		zassert_equal(std::__libcpp_thread_join(&handles[i]), 0);
	}
	zassert_equal(context.failures.load(), 0);
	zassert_equal(context.completed.load(), capacity);
	zassert_equal(replacement_runs.load(), 1);
}

ZTEST(libcxx_thread_stress, test_aborted_detached_slots_are_reusable)
{
	/*
	 * Use a raw entry with no heap allocation or C++ destructors. Abnormal
	 * termination reclaims the kernel slot; it does not run C++ cleanup.
	 */
	for (uint32_t round = 0; round < capacity * 4; ++round) {
		blocked_context context;
		std::__libcpp_thread_t handle = nullptr;

		prepare(context);
		zassert_equal(std::__libcpp_thread_create(&handle, blocked_entry, &context), 0);
		zassert_equal(std::__libcpp_thread_detach(&handle), 0);
		zassert_equal(k_sem_take(&context.entered, K_SECONDS(2)), 0);
		k_thread_abort(handle);
		zassert_equal(k_thread_join(handle, K_SECONDS(2)), 0);
		zassert_equal(context.completed.load(), 0);
	}

	std::atomic<uint32_t> runs{0};
	std::__libcpp_thread_t handle = nullptr;

	zassert_equal(std::__libcpp_thread_create(&handle, increment_entry, &runs), 0);
	zassert_equal(std::__libcpp_thread_join(&handle), 0);
	zassert_equal(runs.load(), 1);
}

ZTEST(libcxx_thread_stress, test_concurrent_creators_and_joiners)
{
	struct k_sem gate;
	std::array<std::thread, creator_count> creators;
	std::atomic<uint32_t> failures{0};
	std::atomic<uint32_t> completed{0};

	k_sem_init(&gate, 0, creator_count);
	for (auto &creator : creators) {
		creator = std::thread([&] {
			if (k_sem_take(&gate, K_SECONDS(2)) != 0) {
				failures.fetch_add(1);
				return;
			}
			for (uint32_t round = 0; round < create_rounds; ++round) {
				std::__libcpp_thread_t child = nullptr;

				if (std::__libcpp_thread_create(&child, increment_entry,
							       &completed) != 0) {
					failures.fetch_add(1);
					continue;
				}
				std::this_thread::yield();
				if (std::__libcpp_thread_join(&child) != 0) {
					failures.fetch_add(1);
				}
			}
		});
	}
	for (uint32_t i = 0; i < creator_count; ++i) {
		k_sem_give(&gate);
	}
	for (auto &creator : creators) {
		creator.join();
	}
	zassert_equal(failures.load(), 0, "Concurrent create/join failed within pool capacity");
	zassert_equal(completed.load(), creator_count * create_rounds);
}

ZTEST(libcxx_thread_stress, test_language_tls_lifo_before_tss_and_reuse)
{
	uint32_t marker = 4;

	for (uint32_t round = 0; round < 32; ++round) {
		std::__libcpp_thread_t handle = nullptr;

		exit_order_size = 0;
		zassert_equal(std::__libcpp_thread_create(&handle, ordered_tls_entry, &marker), 0);
		zassert_equal(std::__libcpp_thread_join(&handle), 0);
		zassert_equal(exit_order_size, 4);
		zassert_equal(exit_order[0], 2, "Language TLS destruction is not LIFO");
		zassert_equal(exit_order[1], 3, "Destructor-time registration was lost");
		zassert_equal(exit_order[2], 1);
		zassert_equal(exit_order[3], 4, "TSS ran before language TLS destruction");
	}
	zassert_equal(destructor_failures.load(), 0);
}

ZTEST(libcxx_thread_stress, test_native_root_explicit_cleanup)
{
	uint32_t marker = 4;

	exit_order_size = 0;
	k_tid_t thread = k_thread_create(&native_root_thread, native_root_stack,
					K_THREAD_STACK_SIZEOF(native_root_stack), native_root_entry,
					&marker, nullptr, nullptr, 5, 0, K_NO_WAIT);

	zassert_equal(k_thread_join(thread, K_SECONDS(2)), 0);
	zassert_equal(exit_order_size, 4, "Native root cleanup did not run all destructors");
	zassert_equal(exit_order[0], 2);
	zassert_equal(exit_order[1], 3);
	zassert_equal(exit_order[2], 1);
	zassert_equal(exit_order[3], 4);
	zassert_equal(destructor_failures.load(), 0);
}

ZTEST(libcxx_thread_stress, test_tss_repopulation_has_four_pass_limit)
{
	for (uint32_t round = 0; round < 32; ++round) {
		rearm_context context;
		std::__libcpp_thread_t handle = nullptr;

		zassert_equal(std::__libcpp_thread_create(&handle, rearming_tls_entry, &context), 0);
		zassert_equal(std::__libcpp_thread_join(&handle), 0);
		zassert_equal(context.calls, 4, "TSS destructor iteration limit changed");
		zassert_equal(context.failures, 0, "TSS was not cleared before callback or reuse");
	}
}

ZTEST(libcxx_thread_stress, test_concurrent_first_use_and_object_lifetime_reuse)
{
	for (uint32_t round = 0; round < 24; ++round) {
		std::mutex mutex;
		std::condition_variable cv;
		struct k_sem gate;
		std::array<std::thread, creator_count> workers;
		std::atomic<uint32_t> failures{0};
		uint32_t arrivals = 0;

		k_sem_init(&gate, 0, creator_count);
		for (auto &worker : workers) {
			worker = std::thread([&] {
				if (k_sem_take(&gate, K_SECONDS(2)) != 0) {
					failures.fetch_add(1);
					return;
				}
				/* Simultaneous notify races the first mutex/CV wait. */
				cv.notify_all();
				std::unique_lock<std::mutex> lock(mutex);

				++arrivals;
				cv.notify_all();
				if (!cv.wait_for(lock, 2s, [&] {
					    return arrivals == creator_count;
				    })) {
					failures.fetch_add(1);
				}
			});
		}
		for (uint32_t i = 0; i < creator_count; ++i) {
			k_sem_give(&gate);
		}
		for (auto &worker : workers) {
			worker.join();
		}
		zassert_equal(failures.load(), 0, "First use or reused object lost a wakeup");
		zassert_equal(arrivals, creator_count);
	}
}

ZTEST(libcxx_thread_stress, test_timeout_reacquires_contended_mutex)
{
	std::mutex mutex;
	std::condition_variable cv;
	struct k_sem gate;
	std::atomic<bool> holder_locked{false};
	std::atomic<uint32_t> failures{0};

	k_sem_init(&gate, 0, 1);
	std::unique_lock<std::mutex> lock(mutex);
	std::thread holder([&] {
		if (k_sem_take(&gate, K_SECONDS(2)) != 0) {
			failures.fetch_add(1);
			return;
		}
		std::lock_guard<std::mutex> guard(mutex);

		holder_locked.store(true);
		std::this_thread::sleep_for(40ms);
	});
	const auto before = std::chrono::steady_clock::now();

	k_sem_give(&gate);
	const auto status = cv.wait_for(lock, 10ms);
	const auto elapsed = std::chrono::steady_clock::now() - before;
	bool acquired_elsewhere = false;
	std::thread probe([&] {
		acquired_elsewhere = mutex.try_lock();
		if (acquired_elsewhere) {
			mutex.unlock();
		}
	});

	probe.join();
	lock.unlock();
	holder.join();

	zassert_equal(failures.load(), 0);
	zassert_true(holder_locked.load());
	zassert_true(status == std::cv_status::timeout);
	zassert_true(elapsed >= 40ms, "Timed wait returned without waiting to reacquire");
	zassert_false(acquired_elsewhere, "Timed wait returned without owning its mutex");
}


ZTEST(libcxx_thread_stress, test_sleep_for_subtick_wakeup_does_not_return_early)
{
	/*
	 * Start late in a tick and wake at its next boundary. Counting whole
	 * uptime ticks alone can mistake a fraction of a tick for a full delay.
	 * The cycle clock measures the actual elapsed duration independently.
	 */
	const uint64_t requested_ns = k_ticks_to_ns_floor64(1);
	const uint64_t requested_cycles = k_ns_to_cyc_ceil64(requested_ns);
	const uint64_t phase_cycles = k_ticks_to_cyc_floor64(1) * 3U / 4U;
	uint32_t exercised = 0;

	zassert_true(requested_cycles > 100U && requested_cycles < UINT32_MAX / 8U,
		     "Cycle clock cannot measure this test's tick interval");
	atomic_ptr_set(&sleep_wake_target, k_current_get());

	for (uint32_t attempt = 0; attempt < 16U && exercised < 3U; ++attempt) {
		atomic_set(&sleep_wake_done, 0);
		const int64_t target_tick = k_uptime_ticks() + 2;

		k_timer_start(&sleep_wake_timer, K_TIMEOUT_ABS_TICKS(target_tick + 1), K_NO_WAIT);
		while (k_uptime_ticks() < target_tick) {
			/* Interrupts remain enabled so the timer can advance. */
		}
		const uint32_t edge_cycles = k_cycle_get_32();

		while (static_cast<uint32_t>(k_cycle_get_32() - edge_cycles) < phase_cycles) {
		}
		const bool phase_valid = k_uptime_ticks() == target_tick;
		uint32_t before = 0;
		uint32_t after = 0;

		if (phase_valid) {
			before = k_cycle_get_32();
			std::this_thread::sleep_for(std::chrono::nanoseconds(requested_ns));
			after = k_cycle_get_32();
		}

		const int64_t limit = k_uptime_ticks() + k_sec_to_ticks_ceil64(2);

		while (atomic_get(&sleep_wake_done) == 0 && k_uptime_ticks() < limit) {
			k_yield();
		}
		zassert_equal(atomic_get(&sleep_wake_done), 1, "Controlled wakeup did not run");
		k_timer_stop(&sleep_wake_timer);

		const uint32_t wake_cycle = static_cast<uint32_t>(atomic_get(&sleep_wake_cycle));

		/*
		 * Host scheduling can miss the intended phase on QEMU SMP. Such
		 * attempts are not evidence: require three actual early wakeups.
		 */
		if (!phase_valid || atomic_get(&sleep_wake_was_sleeping) == 0 ||
		    static_cast<uint32_t>(wake_cycle - before) >= requested_cycles) {
			continue;
		}
		++exercised;
		zassert_true(static_cast<uint32_t>(after - before) >= requested_cycles,
			     "sleep_for returned before its requested duration after k_wakeup");
	}
	zassert_equal(exercised, 3U, "Could not exercise the sub-tick wakeup window");
}

ZTEST_SUITE(libcxx_thread_stress, NULL, setup, NULL, NULL, NULL);
