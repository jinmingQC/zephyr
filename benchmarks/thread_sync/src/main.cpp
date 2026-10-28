/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

static_assert(__cplusplus >= 201703L, "The benchmark requires C++17");
BUILD_ASSERT(CONFIG_CPP_RUNTIME_THREAD_MAX >= 2);
BUILD_ASSERT(CONFIG_MAIN_THREAD_PRIORITY == CONFIG_CPP_RUNTIME_THREAD_PRIORITY);

namespace
{

constexpr unsigned int sample_count = 9;
constexpr unsigned int lock_iterations = 10000;
constexpr unsigned int first_use_iterations = 1000;
constexpr unsigned int contention_iterations = 2000;
constexpr unsigned int ping_pong_iterations = 200;
constexpr unsigned int thread_iterations = 32;
constexpr unsigned int once_iterations = 10000;
constexpr unsigned int p90_index = (sample_count * 9U + 9U) / 10U - 1U;

bool failed;
std::uint64_t counter_hz;

struct measurement {
	std::uint32_t ticks;
	unsigned int operations;
	bool valid;
};

bool check(bool condition, const char *message)
{
	if (!condition) {
		std::printf("BENCH_ERROR,%s\n", message);
		failed = true;
	}
	return condition;
}

template <typename Operation> measurement measure(unsigned int operations, Operation operation)
{
	const std::int64_t before_ms = k_uptime_get();
	const std::uint32_t before_ticks = k_cycle_get_32();

	operation();

	const std::uint32_t elapsed_ticks = k_cycle_get_32() - before_ticks;
	const std::int64_t elapsed_ms = k_uptime_get() - before_ms;
	const std::uint64_t wrap_ms = (UINT64_C(0xffffffff) * 1000U) / counter_hz;
	const std::uint64_t tick_ms = DIV_ROUND_UP(1000U, CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	const bool valid =
		elapsed_ms >= 0 && static_cast<std::uint64_t>(elapsed_ms) + 2U * tick_ms < wrap_ms;

	check(valid, "sample exceeds safe 32-bit timer interval");
	return {elapsed_ticks, operations, valid};
}

struct start_gate {
	std::mutex mutex;
	std::condition_variable cv;
	unsigned int ready = 0;
	bool open = false;

	void wait()
	{
		std::unique_lock<std::mutex> lock(mutex);

		++ready;
		cv.notify_all();
		cv.wait(lock, [this] { return open; });
	}

	void await_ready()
	{
		std::unique_lock<std::mutex> lock(mutex);

		cv.wait(lock, [this] { return ready == 2; });
	}

	void release()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);

			open = true;
		}
		cv.notify_all();
	}
};

measurement mutex_uncontended()
{
	std::mutex mutex;

	/* Initialize any lazy backend object before timing this warmed operation. */
	mutex.lock();
	mutex.unlock();
	return measure(lock_iterations, [&] {
		for (unsigned int i = 0; i < lock_iterations; ++i) {
			mutex.lock();
			mutex.unlock();
		}
	});
}

measurement mutex_first_use()
{
	return measure(first_use_iterations, [] {
		for (unsigned int i = 0; i < first_use_iterations; ++i) {
			std::mutex mutex;
			std::lock_guard<std::mutex> lock(mutex);
		}
	});
}

measurement mutex_contended()
{
	start_gate gate;
	std::mutex mutex;
	unsigned int updates = 0;
	std::array<std::thread, 2> workers;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			gate.wait();
			for (unsigned int i = 0; i < contention_iterations; ++i) {
				std::lock_guard<std::mutex> lock(mutex);

				++updates;
			}
		});
	}
	gate.await_ready();
	const measurement result = measure(2U * contention_iterations, [&] {
		gate.release();
		for (auto &worker : workers) {
			worker.join();
		}
	});

	check(updates == 2U * contention_iterations, "protected update count");
	return result;
}

measurement condition_round_trip()
{
	std::mutex mutex;
	std::condition_variable cv;
	bool worker_ready = false;
	bool start = false;
	unsigned int turn = 0;
	unsigned int replies = 0;
	std::thread worker([&] {
		std::unique_lock<std::mutex> lock(mutex);

		worker_ready = true;
		cv.notify_all();
		cv.wait(lock, [&] { return start; });
		for (unsigned int i = 0; i < ping_pong_iterations; ++i) {
			cv.wait(lock, [&] { return turn == 1; });
			++replies;
			turn = 0;
			cv.notify_one();
		}
	});

	{
		std::unique_lock<std::mutex> lock(mutex);

		cv.wait(lock, [&] { return worker_ready; });
	}
	const measurement result = measure(ping_pong_iterations, [&] {
		std::unique_lock<std::mutex> lock(mutex);

		start = true;
		cv.notify_all();
		for (unsigned int i = 0; i < ping_pong_iterations; ++i) {
			turn = 1;
			cv.notify_one();
			cv.wait(lock, [&] { return turn == 0; });
		}
	});

	worker.join();
	check(replies == ping_pong_iterations, "condition-variable reply count");
	return result;
}

measurement thread_create_join()
{
	std::atomic<unsigned int> completed{0};
	const measurement result = measure(thread_iterations, [&] {
		for (unsigned int i = 0; i < thread_iterations; ++i) {
			std::thread worker(
				[&] { completed.fetch_add(1, std::memory_order_relaxed); });

			worker.join();
		}
	});

	check(completed.load(std::memory_order_relaxed) == thread_iterations,
	      "created thread completion count");
	return result;
}

measurement call_once_ready()
{
	std::once_flag once;
	unsigned int calls = 0;

	std::call_once(once, [&] { ++calls; });
	const measurement result = measure(once_iterations, [&] {
		for (unsigned int i = 0; i < once_iterations; ++i) {
			std::call_once(once, [&] { ++calls; });
		}
	});

	check(calls == 1, "call_once execution count");
	return result;
}

bool run(const char *name, measurement (*benchmark)())
{
	const measurement warmup = benchmark();

	if (!warmup.valid || failed) {
		return false;
	}
	std::array<std::uint32_t, sample_count> ticks{};
	unsigned int operations = 0;

	for (unsigned int sample = 0; sample < sample_count; ++sample) {
		const measurement result = benchmark();

		if (!result.valid || failed) {
			return false;
		}
		ticks[sample] = result.ticks;
		operations = result.operations;
		std::printf("BENCH_RAW,%s,%u,%" PRIu32 ",%u,%" PRIu64 "\n", name, sample,
			    result.ticks, operations, counter_hz);
	}
	std::sort(ticks.begin(), ticks.end());
	const std::uint32_t median = ticks[sample_count / 2U];
	const std::uint64_t ns_per_operation = k_cyc_to_ns_floor64(median) / operations;

	std::printf("BENCH_SUMMARY,%s,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 "\n", name,
		    sample_count, ticks.front(), median, ticks[p90_index], ns_per_operation);
	return true;
}

bool metadata()
{
	counter_hz = sys_clock_hw_cycles_per_sec();
	if (!check(counter_hz != 0, "zero timer frequency")) {
		return false;
	}
	const int main_priority = k_thread_priority_get(k_current_get());
	int worker_priority = -1;
	std::thread worker([&] { worker_priority = k_thread_priority_get(k_current_get()); });

	worker.join();
	std::printf("META,counter_hz,%" PRIu64 "\n", counter_hz);
	std::printf("META,tick_hz,%u\n", static_cast<unsigned int>(CONFIG_SYS_CLOCK_TICKS_PER_SEC));
	std::printf("META,smp,%u\n", static_cast<unsigned int>(IS_ENABLED(CONFIG_SMP)));
	std::printf("META,cplusplus,%ld\n", static_cast<long>(__cplusplus));
#ifdef __clang__
	std::printf("META,compiler,clang\n");
	std::printf("META,compiler_major,%u\n", static_cast<unsigned int>(__clang_major__));
	std::printf("META,compiler_minor,%u\n", static_cast<unsigned int>(__clang_minor__));
#else
	std::printf("META,compiler,gcc\n");
	std::printf("META,compiler_major,%u\n", static_cast<unsigned int>(__GNUC__));
	std::printf("META,compiler_minor,%u\n", static_cast<unsigned int>(__GNUC_MINOR__));
#endif
	std::printf("META,main_priority,%d\n", main_priority);
	std::printf("META,worker_priority,%d\n", worker_priority);
	std::printf("META,thread_capacity,%u\n",
		    static_cast<unsigned int>(CONFIG_CPP_RUNTIME_THREAD_MAX));
	std::printf("META,thread_stack_request,%u\n",
		    static_cast<unsigned int>(CONFIG_CPP_RUNTIME_THREAD_STACK_SIZE));
	std::printf("META,warmup_batches,1\n");
	std::printf("META,measured_batches,%u\n", sample_count);
	std::printf("META,sizeof_mutex,%zu\n", sizeof(std::mutex));
	std::printf("META,sizeof_condition_variable,%zu\n", sizeof(std::condition_variable));
	std::printf("META,sizeof_once_flag,%zu\n", sizeof(std::once_flag));
	std::printf("META,sizeof_thread,%zu\n", sizeof(std::thread));
	std::printf("META,sizeof_shared_mutex,%zu\n", sizeof(std::shared_mutex));
	return check(main_priority == CONFIG_CPP_RUNTIME_THREAD_PRIORITY,
		     "coordinator priority differs from configured worker priority") &&
	       check(worker_priority == CONFIG_CPP_RUNTIME_THREAD_PRIORITY,
		     "actual worker priority differs between backend policies");
}

} // namespace

int main()
{
	if (!metadata() || !run("mutex_uncontended", mutex_uncontended) ||
	    !run("mutex_first_use", mutex_first_use) || !run("mutex_contended", mutex_contended) ||
	    !run("condition_round_trip", condition_round_trip) ||
	    !run("thread_create_join", thread_create_join) ||
	    !run("call_once_ready", call_once_ready)) {
		std::printf("CPP_BENCH_FAILED\n");
		return 1;
	}
	std::printf("CPP_BENCH_DONE\n");
	return 0;
}
