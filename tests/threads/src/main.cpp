/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <zephyr/ztest.h>

using namespace std::chrono_literals;

BUILD_ASSERT(__cplusplus == 201703L);
BUILD_ASSERT(CONFIG_CPP_RUNTIME_THREAD_MAX >= 4);

namespace
{

constexpr uint32_t worker_count = 4;
constexpr uint32_t iterations = 200;
constexpr uint32_t reuse_count = 32;
constexpr auto wait_limit = 2s;

class start_gate
{
      public:
	bool wait()
	{
		std::unique_lock<std::mutex> lock(mutex);

		++waiting;
		cv.notify_all();
		return cv.wait_for(lock, wait_limit, [this] { return open; });
	}

	bool release(uint32_t expected = 0)
	{
		std::unique_lock<std::mutex> lock(mutex);
		const bool ready = cv.wait_for(lock, wait_limit,
					       [this, expected] { return waiting >= expected; });

		open = true;
		lock.unlock();
		cv.notify_all();
		return ready;
	}

      private:
	std::mutex mutex;
	std::condition_variable cv;
	bool open = false;
	uint32_t waiting = 0;
};

std::atomic<uint32_t> static_constructor_count{0};

struct guarded_value {
	guarded_value()
	{
		static_constructor_count.fetch_add(1);
		std::this_thread::sleep_for(10ms);
		value = 42;
	}

	uint32_t value = 0;
};

guarded_value &get_guarded_value()
{
	static guarded_value value;

	return value;
}

std::atomic<uint32_t> tls_destructor_count{0};

struct tls_value {
	~tls_value()
	{
		tls_destructor_count.fetch_add(1);
	}

	uint32_t value = 0;
};

thread_local tls_value local_value;

} /* namespace */

ZTEST(libcxx_threads, test_native_thread_id_move_join)
{
	const auto native_id = std::this_thread::get_id();
	std::thread::id child_id;
	std::thread child([&child_id] { child_id = std::this_thread::get_id(); });
	const auto original_id = child.get_id();
	std::thread owner(std::move(child));
	const bool moved_from_empty = !child.joinable();
	const bool owner_joinable = owner.joinable();

	owner.join();

	zassert_true(native_id != std::thread::id(), "Native thread has an empty ID");
	zassert_true(native_id == std::this_thread::get_id(), "Native thread ID changed");
	zassert_true(moved_from_empty, "Moved-from thread retains ownership");
	zassert_true(owner_joinable, "Moved-to thread lost ownership");
	zassert_true(child_id == original_id, "Thread reports a different ID");
	zassert_true(child_id != native_id, "Native and child thread IDs collide");
	zassert_false(owner.joinable(), "Joined thread retains ownership");
	zassert_true(owner.get_id() == std::thread::id(), "Joined thread ID was not reset");
}

ZTEST(libcxx_threads, test_mutex_contention)
{
	std::mutex mutex;
	start_gate gate;
	std::array<std::thread, worker_count> workers;
	std::atomic<uint32_t> failed_waits{0};
	uint32_t count = 0;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			if (!gate.wait()) {
				failed_waits.fetch_add(1);
				return;
			}
			for (uint32_t i = 0; i < iterations; ++i) {
				std::lock_guard<std::mutex> lock(mutex);

				++count;
				std::this_thread::yield();
			}
		});
	}
	gate.release();
	for (uint32_t i = 0; i < iterations; ++i) {
		std::lock_guard<std::mutex> lock(mutex);

		++count;
	}
	for (auto &worker : workers) {
		worker.join();
	}

	zassert_equal(failed_waits.load(), 0, "Start gate timed out");
	zassert_equal(count, (worker_count + 1) * iterations, "Mutex lost updates");
}

ZTEST(libcxx_threads, test_shared_ptr_contention)
{
	struct payload {
		explicit payload(std::atomic<uint32_t> &counter) : destroyed(counter)
		{
		}

		~payload()
		{
			destroyed.fetch_add(1);
		}

		std::atomic<uint32_t> visits{0};
		std::atomic<uint32_t> &destroyed;
	};

	std::atomic<uint32_t> destructions{0};
	std::atomic<uint32_t> failed_waits{0};
	auto owner = std::make_shared<payload>(destructions);
	start_gate gate;
	std::array<std::thread, worker_count> workers;

	for (auto &worker : workers) {
		worker = std::thread([&, owner] {
			if (!gate.wait()) {
				failed_waits.fetch_add(1);
				return;
			}
			for (uint32_t i = 0; i < iterations; ++i) {
				auto copy = owner;

				copy->visits.fetch_add(1);
				std::this_thread::yield();
			}
		});
	}
	gate.release();
	for (auto &worker : workers) {
		worker.join();
	}
	const uint32_t visits = owner->visits.load();
	const auto references = owner.use_count();
	const uint32_t early_destructions = destructions.load();

	owner.reset();

	zassert_equal(failed_waits.load(), 0, "Start gate timed out");
	zassert_equal(visits, worker_count * iterations, "Shared object lost updates");
	zassert_equal(references, 1, "Concurrent shared_ptr copies corrupted the reference count");
	zassert_equal(early_destructions, 0, "Shared object was destroyed with a live owner");
	zassert_equal(destructions.load(), 1, "Shared object was not destroyed exactly once");
}

ZTEST(libcxx_threads, test_lock_wrappers)
{
	std::mutex first;
	std::mutex second;
	uint32_t count = 0;
	std::thread worker([&] {
		for (uint32_t i = 0; i < iterations; ++i) {
			std::unique_lock<std::mutex> left(first, std::defer_lock);
			std::unique_lock<std::mutex> right(second, std::defer_lock);

			std::lock(left, right);
			++count;
		}
	});

	for (uint32_t i = 0; i < iterations; ++i) {
		std::scoped_lock lock(second, first);

		++count;
	}
	worker.join();
	zassert_equal(count, 2 * iterations, "Multiple-mutex locking lost updates");
}

ZTEST(libcxx_threads, test_recursive_mutex)
{
	std::recursive_mutex mutex;
	bool acquired = false;

	mutex.lock();
	mutex.lock();
	std::thread first([&] {
		acquired = mutex.try_lock();
		if (acquired) {
			mutex.unlock();
		}
	});
	first.join();
	const bool blocked_twice = !acquired;

	mutex.unlock();
	std::thread second([&] {
		acquired = mutex.try_lock();
		if (acquired) {
			mutex.unlock();
		}
	});
	second.join();
	const bool blocked_once = !acquired;

	mutex.unlock();
	std::thread third([&] {
		acquired = mutex.try_lock();
		if (acquired) {
			mutex.unlock();
		}
	});
	third.join();

	zassert_true(blocked_twice, "Recursive mutex admitted another owner");
	zassert_true(blocked_once, "Recursive mutex released before final unlock");
	zassert_true(acquired, "Recursive mutex stayed locked after final unlock");
}

ZTEST(libcxx_threads, test_timed_mutex)
{
	std::timed_mutex mutex;
	bool acquired = false;

	mutex.lock();
	std::thread blocked([&] {
		acquired = mutex.try_lock_for(25ms);
		if (acquired) {
			mutex.unlock();
		}
	});
	blocked.join();
	const bool timed_out = !acquired;
	mutex.unlock();

	std::thread available([&] {
		acquired = mutex.try_lock_until(std::chrono::steady_clock::now() + wait_limit);
		if (acquired) {
			mutex.unlock();
		}
	});
	available.join();

	zassert_true(timed_out, "Timed mutex admitted another owner");
	zassert_true(acquired, "Timed mutex did not acquire after release");
}

ZTEST(libcxx_threads, test_recursive_timed_mutex)
{
	std::recursive_timed_mutex mutex;
	bool acquired = false;

	mutex.lock();
	const bool recursive_acquired = mutex.try_lock_for(25ms);
	std::thread blocked([&] {
		acquired = mutex.try_lock_for(25ms);
		if (acquired) {
			mutex.unlock();
		}
	});
	blocked.join();
	if (recursive_acquired) {
		mutex.unlock();
	}
	mutex.unlock();

	zassert_true(recursive_acquired, "Owner failed to relock recursive timed mutex");
	zassert_false(acquired, "Recursive timed mutex admitted another owner");
}

ZTEST(libcxx_threads, test_condition_variable_predicate)
{
	std::mutex mutex;
	std::condition_variable cv;
	bool ready = false;
	std::unique_lock<std::mutex> lock(mutex);
	std::thread worker([&] {
		{
			std::lock_guard<std::mutex> guard(mutex);

			ready = true;
		}
		cv.notify_one();
	});
	const bool woke = cv.wait_for(lock, wait_limit, [&] { return ready; });

	lock.unlock();
	worker.join();

	zassert_true(woke, "Predicate wait missed notification");
}

ZTEST(libcxx_threads, test_condition_variable_timeouts)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::unique_lock<std::mutex> lock(mutex);
	const auto started = std::chrono::steady_clock::now();
	const bool relative = cv.wait_for(lock, 25ms, [] { return false; });
	const auto elapsed = std::chrono::steady_clock::now() - started;
	const auto deadline = std::chrono::steady_clock::now() + 25ms;
	const bool absolute = cv.wait_until(lock, deadline, [] { return false; });
	const auto finished = std::chrono::steady_clock::now();
	const bool realtime =
		cv.wait_until(lock, std::chrono::system_clock::now() + 25ms, [] { return false; });

	zassert_false(relative, "Relative wait reported a satisfied predicate");
	zassert_true(elapsed >= 25ms, "Relative wait returned before its timeout");
	zassert_false(absolute, "Absolute wait reported a satisfied predicate");
	zassert_true(finished >= deadline, "Absolute wait returned before its deadline");
	zassert_false(realtime, "Realtime wait reported a satisfied predicate");
}

ZTEST(libcxx_threads, test_condition_variable_broadcast)
{
	start_gate gate;
	std::array<std::thread, worker_count> workers;
	std::atomic<uint32_t> awoken{0};

	for (auto &worker : workers) {
		worker = std::thread([&] {
			if (gate.wait()) {
				awoken.fetch_add(1);
			}
		});
	}
	const bool waiting = gate.release(worker_count);

	for (auto &worker : workers) {
		worker.join();
	}

	zassert_true(waiting, "Workers did not reach the wait before broadcast");
	zassert_equal(awoken.load(), worker_count, "Broadcast missed waiting workers");
}

ZTEST(libcxx_threads, test_condition_variable_any)
{
	std::recursive_mutex mutex;
	std::condition_variable_any cv;
	bool ready = false;
	std::unique_lock<std::recursive_mutex> lock(mutex);
	std::thread worker([&] {
		{
			std::lock_guard<std::recursive_mutex> guard(mutex);

			ready = true;
		}
		cv.notify_one();
	});
	const bool woke = cv.wait_until(lock, std::chrono::steady_clock::now() + wait_limit,
					[&] { return ready; });

	lock.unlock();
	worker.join();
	zassert_true(woke, "condition_variable_any missed notification");
}

ZTEST(libcxx_threads, test_shared_mutex)
{
	std::shared_mutex mutex;
	bool reader_acquired = false;
	bool writer_acquired = false;

	mutex.lock_shared();
	std::thread reader([&] {
		reader_acquired = mutex.try_lock_shared();
		if (reader_acquired) {
			mutex.unlock_shared();
		}
		writer_acquired = mutex.try_lock();
		if (writer_acquired) {
			mutex.unlock();
		}
	});
	reader.join();
	mutex.unlock_shared();

	const bool admitted_reader = reader_acquired;
	const bool excluded_writer = !writer_acquired;

	mutex.lock();
	std::thread blocked_reader([&] {
		reader_acquired = mutex.try_lock_shared();
		if (reader_acquired) {
			mutex.unlock_shared();
		}
	});
	blocked_reader.join();
	mutex.unlock();

	zassert_true(admitted_reader, "Concurrent shared ownership was rejected");
	zassert_true(excluded_writer, "Writer acquired while reader held the mutex");
	zassert_false(reader_acquired, "Reader acquired while writer held the mutex");
}

ZTEST(libcxx_threads, test_shared_timed_mutex)
{
	std::shared_timed_mutex mutex;
	bool acquired = false;

	mutex.lock_shared();
	std::thread blocked([&] {
		acquired = mutex.try_lock_for(25ms);
		if (acquired) {
			mutex.unlock();
		}
	});
	blocked.join();
	const bool timed_out = !acquired;
	mutex.unlock_shared();

	std::thread available([&] {
		acquired = mutex.try_lock_until(std::chrono::steady_clock::now() + wait_limit);
		if (acquired) {
			mutex.unlock();
		}
	});
	available.join();

	zassert_true(timed_out, "Timed writer acquired while reader held the mutex");
	zassert_true(acquired, "Timed writer did not acquire after readers released");
}

ZTEST(libcxx_threads, test_call_once)
{
	std::once_flag once;
	start_gate gate;
	std::array<std::thread, worker_count> workers;
	std::atomic<uint32_t> initializations{0};
	std::atomic<uint32_t> failures{0};
	uint32_t value = 0;

	auto initialize = [&] {
		initializations.fetch_add(1);
		std::this_thread::sleep_for(10ms);
		value = 42;
	};

	for (auto &worker : workers) {
		worker = std::thread([&] {
			if (!gate.wait()) {
				failures.fetch_add(1);
				return;
			}
			std::call_once(once, initialize);
			if (value != 42) {
				failures.fetch_add(1);
			}
		});
	}
	gate.release();
	std::call_once(once, initialize);
	for (auto &worker : workers) {
		worker.join();
	}

	zassert_equal(initializations.load(), 1, "call_once initialized more than once");
	zassert_equal(failures.load(), 0, "call_once did not publish initialized state");
}

ZTEST(libcxx_threads, test_static_local_initialization)
{
	start_gate gate;
	std::array<std::thread, worker_count> workers;
	std::atomic<uint32_t> failures{0};

	for (auto &worker : workers) {
		worker = std::thread([&] {
			if (!gate.wait() || get_guarded_value().value != 42) {
				failures.fetch_add(1);
			}
		});
	}
	gate.release();
	const uint32_t native_value = get_guarded_value().value;
	for (auto &worker : workers) {
		worker.join();
	}

	zassert_equal(native_value, 42, "Native thread read a partially constructed static");
	zassert_equal(static_constructor_count.load(), 1, "Static constructed more than once");
	zassert_equal(failures.load(), 0, "Static initialization did not synchronize workers");
}

ZTEST(libcxx_threads, test_thread_local_destruction)
{
	start_gate gate;
	std::array<std::thread, worker_count> workers;
	std::atomic<uint32_t> failures{0};
	const uint32_t before = tls_destructor_count.load();

	for (uint32_t i = 0; i < worker_count; ++i) {
		workers[i] = std::thread([&, i] {
			local_value.value = i + 1;
			if (!gate.wait()) {
				failures.fetch_add(1);
			}
			std::this_thread::yield();
			if (local_value.value != i + 1) {
				failures.fetch_add(1);
			}
		});
	}
	gate.release();
	for (auto &worker : workers) {
		worker.join();
	}

	zassert_equal(failures.load(), 0, "TLS values were shared or start gate timed out");
	zassert_equal(tls_destructor_count.load() - before, worker_count,
		      "join returned before TLS destructors completed");
}

ZTEST(libcxx_threads, test_notify_all_at_thread_exit)
{
	std::mutex mutex;
	std::condition_variable cv;
	bool ready = false;
	const uint32_t before = tls_destructor_count.load();
	std::unique_lock<std::mutex> lock(mutex);
	std::thread worker([&] {
		local_value.value = 42;
		std::unique_lock<std::mutex> guard(mutex);

		ready = true;
		std::notify_all_at_thread_exit(cv, std::move(guard));
	});
	const bool woke = cv.wait_for(lock, wait_limit, [&] { return ready; });
	const uint32_t destructed_at_notification = tls_destructor_count.load() - before;

	lock.unlock();
	worker.join();

	zassert_true(woke, "Thread-exit notification was lost");
	zassert_equal(destructed_at_notification, 1,
		      "Thread-exit notification preceded TLS destruction");
}

ZTEST(libcxx_threads, test_thread_reuse)
{
	uint32_t completed = 0;

	for (uint32_t i = 0; i < reuse_count; ++i) {
		std::thread worker([&] { ++completed; });

		worker.join();
	}
	zassert_equal(completed, reuse_count, "Joined thread resources were not reusable");
}

ZTEST(libcxx_threads, test_detach_reuse)
{
	for (uint32_t i = 0; i < reuse_count; ++i) {
		auto completion = std::make_shared<std::promise<void>>();
		auto done = completion->get_future();
		std::thread worker([completion] { completion->set_value_at_thread_exit(); });

		worker.detach();
		zassert_false(worker.joinable(), "Detached thread retains ownership");
		zassert_true(worker.get_id() == std::thread::id(),
			     "Detached thread ID was not reset");
		const auto status = done.wait_for(wait_limit);

		zassert_true(status == std::future_status::ready,
			     "Detached thread did not complete");
		done.get();
	}
}

ZTEST(libcxx_threads, test_future_promise)
{
	std::promise<uint32_t> promise;
	auto future = promise.get_future();
	std::thread worker([promise = std::move(promise)]() mutable { promise.set_value(42); });
	const auto status = future.wait_for(wait_limit);

	worker.join();
	zassert_true(status == std::future_status::ready, "Promise result did not become ready");
	zassert_equal(future.get(), 42, "Promise result was corrupted");
}

ZTEST(libcxx_threads, test_async)
{
	auto asynchronous = std::async(std::launch::async, [] { return uint32_t{42}; });
	const auto status = asynchronous.wait_for(wait_limit);

	zassert_true(status == std::future_status::ready, "Async result did not become ready");
	zassert_equal(asynchronous.get(), 42, "Async result was corrupted");

	auto deferred = std::async(std::launch::deferred, [] { return uint32_t{17}; });

	zassert_true(deferred.wait_for(0ms) == std::future_status::deferred,
		     "Deferred task ran eagerly");
	zassert_equal(deferred.get(), 17, "Deferred result was corrupted");
}

ZTEST(libcxx_threads, test_sleep_for)
{
	const auto started = std::chrono::steady_clock::now();

	std::this_thread::sleep_for(10ms);
	zassert_true(std::chrono::steady_clock::now() - started >= 10ms,
		     "sleep_for returned before its duration");
}

ZTEST_SUITE(libcxx_threads, NULL, NULL, NULL, NULL, NULL);
