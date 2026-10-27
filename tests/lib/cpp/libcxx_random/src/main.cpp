/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <system_error>
#include <thread>

#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

BUILD_ASSERT(__cplusplus == 201703L);
BUILD_ASSERT(CONFIG_CPP_EXCEPTIONS);
BUILD_ASSERT(CONFIG_LIBCXX_RANDOM_DEVICE);
BUILD_ASSERT(CONFIG_TEST_CSPRNG_GENERATOR);

namespace
{

using result_type = std::random_device::result_type;

constexpr result_type injected_value = 0xa5963c71U;
std::atomic<uint32_t> requests{0};
std::atomic<size_t> requested_bytes{0};
std::atomic<uint32_t> invalid_requests{0};
std::atomic<int> injected_error{0};
std::atomic<result_type> next_value{injected_value};

void reset_entropy(void *)
{
	requests.store(0);
	requested_bytes.store(0);
	invalid_requests.store(0);
	injected_error.store(0);
	next_value.store(injected_value);
}

} /* namespace */

/*
 * Supervisor-mode sys_csrand_get() calls this implementation symbol through
 * its generated inline syscall wrapper. Only this test image redirects it;
 * the deterministic bytes exercise routing and errors, not entropy quality.
 */
extern "C" int __wrap_z_impl_sys_csrand_get(void *destination, size_t length)
{
	requests.fetch_add(1);
	requested_bytes.fetch_add(length);
	if (destination == nullptr || length != sizeof(result_type)) {
		invalid_requests.fetch_add(1);
		return -EINVAL;
	}
	const int error = injected_error.load();

	if (error != 0) {
		return error;
	}
	const result_type value = next_value.load();

	std::memcpy(destination, &value, sizeof(value));
	return 0;
}

ZTEST(libcxx_random, test_default_device_routes_exact_native_result)
{
	std::random_device device;

	zassert_equal(device(), injected_value);
	zassert_equal(requests.load(), 1);
	zassert_equal(requested_bytes.load(), sizeof(result_type));
	zassert_equal(invalid_requests.load(), 0);
}

ZTEST(libcxx_random, test_explicit_default_token)
{
	std::random_device device("/dev/urandom");

	zassert_equal(device(), injected_value);
	zassert_equal(requests.load(), 1);
	zassert_equal(invalid_requests.load(), 0);
}

ZTEST(libcxx_random, test_repeated_requests_do_not_cache)
{
	std::random_device device;

	zassert_equal(device(), injected_value);
	next_value.store(0);
	zassert_equal(device(), 0);
	next_value.store(std::numeric_limits<result_type>::max());
	zassert_equal(device(), std::numeric_limits<result_type>::max());
	zassert_equal(requests.load(), 3);
	zassert_equal(requested_bytes.load(), 3U * sizeof(result_type));
	zassert_equal(invalid_requests.load(), 0);
}

ZTEST(libcxx_random, test_invalid_token_throws_before_requesting_entropy)
{
	bool caught = false;

	try {
		std::random_device device("/libcxx-invalid-entropy-source");

		(void)device;
	} catch (const std::system_error &error) {
		caught = error.code() == std::errc::no_such_file_or_directory &&
			 error.code().value() == ENOENT;
	}
	zassert_true(caught);
	zassert_equal(requests.load(), 0);
	zassert_equal(requested_bytes.load(), 0);
}

ZTEST(libcxx_random, test_native_failure_propagates_and_recovers)
{
	std::random_device device;
	bool caught = false;

	injected_error.store(-EIO);
	try {
		(void)device();
	} catch (const std::system_error &error) {
		caught = error.code() == std::errc::io_error && error.code().value() == EIO;
	}
	zassert_true(caught);
	zassert_equal(requests.load(), 1);
	zassert_equal(requested_bytes.load(), sizeof(result_type));
	zassert_equal(invalid_requests.load(), 0);

	injected_error.store(0);
	zassert_equal(device(), injected_value);
	zassert_equal(requests.load(), 2);
	zassert_equal(requested_bytes.load(), 2U * sizeof(result_type));
}

ZTEST(libcxx_random, test_unknown_entropy_estimate_and_result_range)
{
	std::random_device device;

	zassert_true(device.entropy() == 0.0);
	zassert_equal(device.min(), 0);
	zassert_equal(device.max(), std::numeric_limits<result_type>::max());
}

ZTEST(libcxx_random, test_concurrent_devices)
{
	constexpr uint32_t iterations = 32;
	std::atomic<bool> start{false};
	std::atomic<uint32_t> failures{0};
	std::array<std::thread, 4> workers;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			try {
				std::random_device device;

				while (!start.load()) {
					std::this_thread::yield();
				}
				for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
					if (device() != injected_value) {
						failures.fetch_add(1);
					}
					std::this_thread::yield();
				}
			} catch (...) {
				failures.fetch_add(1);
			}
		});
	}
	start.store(true);
	for (auto &worker : workers) {
		worker.join();
	}
	const size_t expected_requests = workers.size() * iterations;

	zassert_equal(failures.load(), 0);
	zassert_equal(requests.load(), expected_requests);
	zassert_equal(requested_bytes.load(), expected_requests * sizeof(result_type));
	zassert_equal(invalid_requests.load(), 0);
}

ZTEST_SUITE(libcxx_random, nullptr, nullptr, reset_entropy, nullptr, nullptr);
