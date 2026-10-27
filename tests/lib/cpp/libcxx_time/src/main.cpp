/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <thread>

#include <zephyr/ztest.h>

using namespace std::chrono_literals;
using std::chrono::duration_cast;
using std::chrono::nanoseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::chrono::system_clock;

BUILD_ASSERT(__cplusplus == 201703L);
BUILD_ASSERT(steady_clock::is_steady);

namespace
{

constexpr std::time_t leap_day_time = 1709210096;
constexpr auto sleep_interval = 20ms;

struct timespec saved_realtime;
steady_clock::time_point saved_steady;
bool restore_realtime;

nanoseconds as_duration(const struct timespec &value)
{
	return seconds(value.tv_sec) + nanoseconds(value.tv_nsec);
}

void time_before(void *)
{
	restore_realtime = false;
	zassert_equal(clock_gettime(CLOCK_REALTIME, &saved_realtime), 0,
		      "Cannot save realtime clock");
	saved_steady = steady_clock::now();
	restore_realtime = true;
}

void time_after(void *)
{
	if (!restore_realtime) {
		return;
	}
	const auto restored = as_duration(saved_realtime) + (steady_clock::now() - saved_steady);
	const auto whole_seconds = duration_cast<seconds>(restored);
	struct timespec value = {};

	value.tv_sec = static_cast<std::time_t>(whole_seconds.count());
	value.tv_nsec = static_cast<long>((restored - whole_seconds).count());
	zassert_equal(clock_settime(CLOCK_REALTIME, &value), 0, "Cannot restore realtime clock");
	restore_realtime = false;
}

void set_wall_clock(std::time_t seconds_value, long nanoseconds_value = 0)
{
	struct timespec value = {};

	value.tv_sec = seconds_value;
	value.tv_nsec = nanoseconds_value;
	zassert_equal(clock_settime(CLOCK_REALTIME, &value), 0, "Cannot set realtime clock");
}

} /* namespace */

ZTEST(libcxx_time, test_duration_cast_and_rounding)
{
	using std::chrono::ceil;
	using std::chrono::floor;
	using std::chrono::milliseconds;
	using std::chrono::round;

	static_assert(duration_cast<milliseconds>(2s).count() == 2000);
	static_assert(duration_cast<seconds>(1500ms).count() == 1);
	static_assert(duration_cast<seconds>(-1500ms).count() == -1);
	static_assert(floor<seconds>(-1500ms).count() == -2);
	static_assert(ceil<seconds>(-1500ms).count() == -1);
	static_assert(round<seconds>(1500ms).count() == 2);
	static_assert(round<seconds>(2500ms).count() == 2);

	const auto interval = 3s + 250ms + 750us;

	zassert_equal(duration_cast<milliseconds>(interval).count(), 3250);
	zassert_equal(duration_cast<nanoseconds>(interval).count(), 3250750000LL);
	zassert_true(std::chrono::abs(-interval) == interval);
}

ZTEST(libcxx_time, test_time_point_arithmetic)
{
	const auto origin = system_clock::from_time_t(leap_day_time);
	const auto later = origin + 1500ms;
	const auto rounded = std::chrono::floor<seconds>(later);
	const auto ceiling = std::chrono::ceil<seconds>(later);

	zassert_true(later - origin == 1500ms);
	zassert_true(rounded - origin == 1s);
	zassert_true(ceiling - origin == 2s);
	zassert_true(later > origin);
	zassert_true(later - 1500ms == origin);
}

ZTEST(libcxx_time, test_time_t_round_trip)
{
	const auto point = system_clock::from_time_t(leap_day_time);

	zassert_equal(system_clock::to_time_t(point), leap_day_time);
	zassert_equal(system_clock::to_time_t(point + 999ms), leap_day_time);
	zassert_equal(system_clock::to_time_t(point + 1s), leap_day_time + 1);
	zassert_true(system_clock::from_time_t(system_clock::to_time_t(point)) == point);
}

ZTEST(libcxx_time, test_steady_clock_monotonic)
{
	auto previous = steady_clock::now();

	for (uint32_t i = 0; i < 256; ++i) {
		const auto current = steady_clock::now();

		zassert_true(current >= previous, "steady_clock moved backwards");
		previous = current;
		std::this_thread::yield();
	}
}

ZTEST(libcxx_time, test_steady_clock_native_mapping)
{
	struct timespec before;
	struct timespec after;

	zassert_equal(clock_gettime(CLOCK_MONOTONIC, &before), 0);
	const auto sample = steady_clock::now().time_since_epoch();
	zassert_equal(clock_gettime(CLOCK_MONOTONIC, &after), 0);

	zassert_true(sample >= as_duration(before), "steady_clock precedes native clock");
	zassert_true(sample <= as_duration(after), "steady_clock exceeds native clock");
}

ZTEST(libcxx_time, test_system_clock_native_mapping)
{
	set_wall_clock(leap_day_time, 345000000);

	struct timespec before;
	struct timespec after;

	zassert_equal(clock_gettime(CLOCK_REALTIME, &before), 0);
	const auto sample = system_clock::now().time_since_epoch();
	zassert_equal(clock_gettime(CLOCK_REALTIME, &after), 0);

	/* libc++ system_clock stores microseconds and truncates sub-microsecond digits. */
	zassert_true(sample + 1us >= as_duration(before),
		     "system_clock did not follow CLOCK_REALTIME");
	zassert_true(sample <= as_duration(after), "system_clock exceeds native clock");
	zassert_true(sample >= seconds(leap_day_time), "Wall-clock epoch was lost");
}

ZTEST(libcxx_time, test_wall_clock_steps_preserve_steady_clock)
{
	set_wall_clock(leap_day_time);

	const auto steady_before = steady_clock::now();
	const auto wall_before = system_clock::now();

	set_wall_clock(leap_day_time + 86400);
	const auto forward = system_clock::now();
	const auto steady_forward = steady_clock::now();

	set_wall_clock(leap_day_time - 86400);
	const auto backward = system_clock::now();
	const auto steady_backward = steady_clock::now();

	zassert_true(forward - wall_before > 23h, "Forward realtime step was ignored");
	zassert_true(wall_before - backward > 23h, "Backward realtime step was ignored");
	zassert_true(steady_forward >= steady_before, "Forward step reversed steady_clock");
	zassert_true(steady_backward >= steady_forward, "Backward step reversed steady_clock");
	zassert_true(steady_backward - steady_before < 2s,
		     "Realtime adjustment stepped the monotonic clock");
}

ZTEST(libcxx_time, test_sleep_for)
{
	const auto before = steady_clock::now();

	std::this_thread::sleep_for(sleep_interval);

	zassert_true(steady_clock::now() - before >= sleep_interval, "sleep_for returned early");
}

ZTEST(libcxx_time, test_sleep_until_steady)
{
	const auto deadline = steady_clock::now() + sleep_interval;

	std::this_thread::sleep_until(deadline);

	zassert_true(steady_clock::now() >= deadline,
		     "sleep_until returned before steady deadline");
}

ZTEST(libcxx_time, test_sleep_until_system)
{
	set_wall_clock(leap_day_time);
	const auto before = steady_clock::now();
	const auto deadline = system_clock::now() + sleep_interval;

	std::this_thread::sleep_until(deadline);

	zassert_true(system_clock::now() >= deadline, "sleep_until returned before wall deadline");
	zassert_true(steady_clock::now() - before >= sleep_interval,
		     "Wall-clock sleep skipped the requested interval");
}

ZTEST(libcxx_time, test_nonpositive_sleep_and_past_deadlines)
{
	const auto before = steady_clock::now();

	std::this_thread::sleep_for(0ns);
	std::this_thread::sleep_for(-1s);
	std::this_thread::sleep_until(before - 1s);
	std::this_thread::sleep_until(system_clock::now() - 1s);

	zassert_true(steady_clock::now() - before < 1s,
		     "Nonpositive sleep or an expired deadline blocked");
}

ZTEST(libcxx_time, test_condition_variable_steady_deadline)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::unique_lock<std::mutex> lock(mutex);
	const auto deadline = steady_clock::now() + sleep_interval;
	const bool signaled = cv.wait_until(lock, deadline, [] { return false; });

	zassert_false(signaled, "A false predicate was reported ready");
	zassert_true(steady_clock::now() >= deadline, "Steady deadline expired early");
}

ZTEST(libcxx_time, test_condition_variable_system_deadline)
{
	set_wall_clock(leap_day_time, 900000000);

	std::mutex mutex;
	std::condition_variable cv;
	std::unique_lock<std::mutex> lock(mutex);
	const auto deadline = system_clock::now() + 150ms;
	const bool signaled = cv.wait_until(lock, deadline, [] { return false; });

	zassert_false(signaled, "A false predicate was reported ready");
	zassert_true(system_clock::now() >= deadline, "Wall deadline expired early");
}

ZTEST(libcxx_time, test_time_and_system_clock_agree)
{
	set_wall_clock(leap_day_time);
	const auto before = system_clock::to_time_t(system_clock::now());
	std::time_t stored = 0;
	const auto reported = std::time(&stored);
	const auto after = system_clock::to_time_t(system_clock::now());

	zassert_equal(stored, reported, "time did not fill its output argument");
	zassert_true(reported >= before && reported <= after, "time uses a different epoch");
	zassert_equal(std::difftime(leap_day_time + 90, leap_day_time), 90.0);
}

ZTEST(libcxx_time, test_utc_calendar_and_formatting)
{
	const std::time_t timestamp = leap_day_time;
	const std::tm *utc = std::gmtime(&timestamp);

	zassert_not_null(utc, "gmtime failed");

	const std::tm copy = *utc;
	char formatted[32] = {};
	const size_t length =
		std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &copy);

	zassert_equal(copy.tm_year, 124);
	zassert_equal(copy.tm_mon, 1);
	zassert_equal(copy.tm_mday, 29);
	zassert_equal(copy.tm_hour, 12);
	zassert_equal(copy.tm_min, 34);
	zassert_equal(copy.tm_sec, 56);
	zassert_equal(copy.tm_wday, 4);
	zassert_equal(copy.tm_yday, 59);
	zassert_equal(length, 19);
	zassert_mem_equal(formatted, "2024-02-29 12:34:56", 20);
}

ZTEST(libcxx_time, test_local_calendar_round_trip)
{
	const std::time_t timestamp = leap_day_time;
	const std::tm *local = std::localtime(&timestamp);

	zassert_not_null(local, "localtime failed");

	std::tm copy = *local;

	zassert_equal(std::mktime(&copy), timestamp, "Local calendar did not round-trip");
}

ZTEST_SUITE(libcxx_time, nullptr, nullptr, time_before, time_after, nullptr);
