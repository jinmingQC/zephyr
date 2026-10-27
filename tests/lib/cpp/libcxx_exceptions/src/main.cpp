/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <any>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

BUILD_ASSERT(__cplusplus == 201703L);
BUILD_ASSERT(CONFIG_CPP_EXCEPTIONS);
BUILD_ASSERT(CONFIG_CPP_RTTI);

namespace
{

struct payload_error: std::runtime_error {
	explicit payload_error(int value) : std::runtime_error("payload"), value(value)
	{
	}

	int value;
};

struct destruction_log {
	std::array<int, 4> order{};
	size_t count = 0;
};

struct stack_guard {
	destruction_log &log;
	int value;

	~stack_guard()
	{
		log.order[log.count++] = value;
	}
};

__noinline void throw_from_leaf(destruction_log &log)
{
	stack_guard guard{log, 3};

	throw payload_error(73);
}

__noinline void throw_from_middle(destruction_log &log)
{
	stack_guard guard{log, 2};

	throw_from_leaf(log);
}

struct polymorphic_base {
	virtual ~polymorphic_base() = default;
};

struct left_base: virtual polymorphic_base {
	int left = 17;
};

struct right_base: virtual polymorphic_base {
	int right = 29;
};

struct diamond: left_base, right_base {
	int value = 42;
};

std::atomic<int> exception_objects{0};

struct counted_error {
	explicit counted_error(int value) : value(value)
	{
		exception_objects.fetch_add(1);
	}

	counted_error(const counted_error &other) : value(other.value)
	{
		exception_objects.fetch_add(1);
	}

	~counted_error()
	{
		exception_objects.fetch_sub(1);
	}

	int value;
};

struct count_unwinding {
	int &observed;

	~count_unwinding()
	{
		observed = std::uncaught_exceptions();
	}
};

struct nested_unwinding {
	int &outer_count;
	int &inner_count;
	int &restored_count;

	~nested_unwinding()
	{
		outer_count = std::uncaught_exceptions();
		try {
			count_unwinding guard{inner_count};

			throw 2;
		} catch (int) {
		}
		restored_count = std::uncaught_exceptions();
	}
};

std::atomic<int> static_attempts{0};

struct retry_static {
	retry_static()
	{
		if (static_attempts.fetch_add(1) == 0) {
			throw payload_error(5);
		}
		value = 64;
	}

	int value;
};

retry_static &get_retry_static()
{
	static retry_static instance;

	return instance;
}

struct construction_counts {
	int members_destroyed = 0;
	int object_destroyed = 0;
};

struct constructor_member {
	construction_counts &counts;

	~constructor_member()
	{
		++counts.members_destroyed;
	}
};

struct failing_constructor {
	explicit failing_constructor(construction_counts &counts) : member{counts}, counts(counts)
	{
		throw payload_error(8);
	}

	~failing_constructor()
	{
		++counts.object_destroyed;
	}

	constructor_member member;
	construction_counts &counts;
};

} /* namespace */

ZTEST(libcxx_exceptions, test_primitive_throw_and_catch)
{
	bool caught = false;

	try {
		throw 42;
	} catch (int value) {
		caught = value == 42;
	}
	zassert_true(caught);
	zassert_equal(std::uncaught_exceptions(), 0);
	zassert_true(std::current_exception() == nullptr);
}

ZTEST(libcxx_exceptions, test_cross_frame_raii_unwinding)
{
	destruction_log log;
	bool caught = false;

	try {
		stack_guard guard{log, 1};

		throw_from_middle(log);
	} catch (const payload_error &error) {
		caught = error.value == 73;
	}
	zassert_true(caught);
	zassert_equal(log.count, 3);
	zassert_equal(log.order[0], 3);
	zassert_equal(log.order[1], 2);
	zassert_equal(log.order[2], 1);
}

ZTEST(libcxx_exceptions, test_derived_exception_caught_as_base)
{
	bool caught = false;

	try {
		throw payload_error(19);
	} catch (const std::exception &base) {
		const auto *derived = dynamic_cast<const payload_error *>(&base);

		caught = derived != nullptr && derived->value == 19 &&
			 std::strcmp(base.what(), "payload") == 0;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_rethrow_preserves_dynamic_type)
{
	bool caught = false;

	try {
		try {
			throw payload_error(51);
		} catch (const std::runtime_error &) {
			throw;
		}
	} catch (const payload_error &error) {
		caught = error.value == 51;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_exception_ptr_owns_exception)
{
	zassert_equal(exception_objects.load(), 0);

	std::exception_ptr saved;

	try {
		throw counted_error(77);
	} catch (...) {
		saved = std::current_exception();
	}
	zassert_true(saved != nullptr);
	zassert_equal(exception_objects.load(), 1);
	for (int attempt = 0; attempt < 3; ++attempt) {
		bool caught = false;

		try {
			std::rethrow_exception(saved);
		} catch (const counted_error &error) {
			caught = error.value == 77;
		}
		zassert_true(caught);
		zassert_equal(exception_objects.load(), 1);
	}
	saved = nullptr;
	zassert_equal(exception_objects.load(), 0);
}

ZTEST(libcxx_exceptions, test_make_exception_ptr)
{
	const auto saved = std::make_exception_ptr(payload_error(11));
	bool caught = false;

	try {
		std::rethrow_exception(saved);
	} catch (const payload_error &error) {
		caught = error.value == 11;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_nested_exception)
{
	bool outer_caught = false;
	bool inner_caught = false;

	try {
		try {
			throw std::logic_error("inner");
		} catch (...) {
			std::throw_with_nested(std::runtime_error("outer"));
		}
	} catch (const std::runtime_error &outer) {
		outer_caught = std::strcmp(outer.what(), "outer") == 0;
		try {
			std::rethrow_if_nested(outer);
		} catch (const std::logic_error &inner) {
			inner_caught = std::strcmp(inner.what(), "inner") == 0;
		}
	}
	zassert_true(outer_caught);
	zassert_true(inner_caught);
}

ZTEST(libcxx_exceptions, test_uncaught_exception_nesting)
{
	int outer_count = -1;
	int inner_count = -1;
	int restored_count = -1;
	bool caught = false;

	try {
		nested_unwinding guard{outer_count, inner_count, restored_count};

		throw 1;
	} catch (int value) {
		caught = value == 1;
	}
	zassert_true(caught);
	zassert_equal(outer_count, 1);
	zassert_equal(inner_count, 2);
	zassert_equal(restored_count, 1);
	zassert_equal(std::uncaught_exceptions(), 0);
}

ZTEST(libcxx_exceptions, test_failed_constructor_cleans_members)
{
	construction_counts counts;
	bool caught = false;

	try {
		auto object = std::make_unique<failing_constructor>(counts);

		(void)object;
	} catch (const payload_error &error) {
		caught = error.value == 8;
	}
	zassert_true(caught);
	zassert_equal(counts.members_destroyed, 1);
	zassert_equal(counts.object_destroyed, 0);
}

ZTEST(libcxx_exceptions, test_rtti_multiple_and_virtual_inheritance)
{
	diamond object;
	polymorphic_base *base = &object;
	left_base *left = &object;
	right_base *right = dynamic_cast<right_base *>(left);

	zassert_true(dynamic_cast<diamond *>(base) == &object);
	zassert_true(right == static_cast<right_base *>(&object));
	zassert_equal(right->right, 29);
	zassert_true(dynamic_cast<void *>(right) == static_cast<void *>(&object));
	zassert_true(typeid(*base) == typeid(diamond));
	zassert_true(typeid(*base) != typeid(polymorphic_base));
	zassert_true(typeid(*base).name()[0] != '\0');
}

ZTEST(libcxx_exceptions, test_failed_dynamic_cast)
{
	polymorphic_base base;
	polymorphic_base *source = &base;
	bool caught = false;

	zassert_true(dynamic_cast<diamond *>(source) == nullptr);
	try {
		(void)dynamic_cast<diamond &>(*source);
	} catch (const std::bad_cast &) {
		caught = true;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_typeid_null_polymorphic_pointer)
{
	polymorphic_base *base = nullptr;
	bool caught = false;

	try {
		(void)typeid(*base);
	} catch (const std::bad_typeid &) {
		caught = true;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_thread_exception_state_isolation)
{
	std::atomic<bool> start{false};
	std::atomic<uint32_t> failures{0};
	std::array<std::thread, 4> workers;

	for (size_t index = 0; index < workers.size(); ++index) {
		workers[index] = std::thread([&, index] {
			while (!start.load()) {
				std::this_thread::yield();
			}
			for (int iteration = 0; iteration < 32; ++iteration) {
				const int expected = static_cast<int>(index) * 1000 + iteration;
				int unwinding = -1;

				try {
					count_unwinding guard{unwinding};

					throw payload_error(expected);
				} catch (...) {
					const auto saved = std::current_exception();

					std::this_thread::yield();
					if (saved == nullptr || std::uncaught_exceptions() != 0) {
						failures.fetch_add(1);
					}
					try {
						std::rethrow_exception(saved);
					} catch (const payload_error &error) {
						if (error.value != expected) {
							failures.fetch_add(1);
						}
					} catch (...) {
						failures.fetch_add(1);
					}
				}
				if (unwinding != 1 || std::current_exception() != nullptr) {
					failures.fetch_add(1);
				}
			}
		});
	}
	start.store(true);
	for (auto &worker : workers) {
		worker.join();
	}
	zassert_equal(failures.load(), 0);
}

ZTEST(libcxx_exceptions, test_promise_exception)
{
	std::promise<int> promise;
	auto future = promise.get_future();
	bool caught = false;

	promise.set_exception(std::make_exception_ptr(payload_error(23)));
	try {
		(void)future.get();
	} catch (const payload_error &error) {
		caught = error.value == 23;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_async_exception)
{
	for (auto policy : {std::launch::async, std::launch::deferred}) {
		auto future = std::async(policy, []() -> int { throw payload_error(37); });
		bool caught = false;

		try {
			(void)future.get();
		} catch (const payload_error &error) {
			caught = error.value == 37;
		}
		zassert_true(caught);
	}
}

ZTEST(libcxx_exceptions, test_packaged_task_exception)
{
	std::packaged_task<int()> task([]() -> int { throw payload_error(41); });
	auto future = task.get_future();
	std::thread worker(std::move(task));
	bool caught = false;

	worker.join();
	try {
		(void)future.get();
	} catch (const payload_error &error) {
		caught = error.value == 41;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_shared_future_rethrows)
{
	std::promise<int> promise;
	auto future = promise.get_future().share();

	promise.set_exception(std::make_exception_ptr(payload_error(43)));
	for (int iteration = 0; iteration < 3; ++iteration) {
		bool caught = false;

		try {
			(void)future.get();
		} catch (const payload_error &error) {
			caught = error.value == 43;
		}
		zassert_true(caught);
	}
}

ZTEST(libcxx_exceptions, test_broken_promise)
{
	std::future<int> future;

	{
		std::promise<int> promise;

		future = promise.get_future();
	}
	bool caught = false;

	try {
		(void)future.get();
	} catch (const std::future_error &error) {
		caught = error.code() == std::make_error_code(std::future_errc::broken_promise);
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_call_once_retries_after_exception)
{
	std::once_flag flag;
	int attempts = 0;

	auto initialize = [&] {
		++attempts;
		if (attempts < 3) {
			throw payload_error(attempts);
		}
	};

	for (int expected = 1; expected <= 2; ++expected) {
		bool caught = false;

		try {
			std::call_once(flag, initialize);
		} catch (const payload_error &error) {
			caught = error.value == expected;
		}
		zassert_true(caught);
	}
	std::call_once(flag, initialize);
	std::call_once(flag, initialize);
	zassert_equal(attempts, 3);
}

ZTEST(libcxx_exceptions, test_call_once_waiters_after_exception)
{
	std::once_flag flag;
	std::atomic<int> attempts{0};
	std::atomic<int> caught{0};
	std::atomic<int> completed{0};
	std::array<std::thread, 4> workers;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			try {
				std::call_once(flag, [&] {
					if (attempts.fetch_add(1) == 0) {
						std::this_thread::yield();
						throw payload_error(1);
					}
				});
				completed.fetch_add(1);
			} catch (const payload_error &) {
				caught.fetch_add(1);
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	zassert_equal(attempts.load(), 2);
	zassert_equal(caught.load(), 1);
	zassert_equal(completed.load(), 3);
}

ZTEST(libcxx_exceptions, test_local_static_retries_after_exception)
{
	bool caught = false;

	try {
		(void)get_retry_static();
	} catch (const payload_error &error) {
		caught = error.value == 5;
	}
	zassert_true(caught);
	auto &value = get_retry_static();

	zassert_equal(value.value, 64);
	zassert_true(&get_retry_static() == &value);
	zassert_equal(static_attempts.load(), 2);
}

ZTEST(libcxx_exceptions, test_container_range_and_length_errors)
{
	std::vector<int> values{1, 2, 3};
	bool range_caught = false;
	bool length_caught = false;

	try {
		(void)values.at(3);
	} catch (const std::out_of_range &) {
		range_caught = true;
	}
	try {
		values.reserve(values.max_size() + 1);
	} catch (const std::length_error &) {
		length_caught = true;
	}
	zassert_true(range_caught);
	zassert_true(length_caught);
	zassert_equal(values.size(), 3);
	zassert_equal(values[2], 3);
}

ZTEST(libcxx_exceptions, test_optional_variant_any_errors)
{
	int caught = 0;
	std::optional<int> optional;
	std::variant<int, std::string> variant(5);
	std::any any(7);

	try {
		(void)optional.value();
	} catch (const std::bad_optional_access &) {
		++caught;
	}
	try {
		(void)std::get<std::string>(variant);
	} catch (const std::bad_variant_access &) {
		++caught;
	}
	try {
		(void)std::any_cast<std::string>(any);
	} catch (const std::bad_any_cast &) {
		++caught;
	}
	zassert_equal(caught, 3);
}

ZTEST(libcxx_exceptions, test_empty_function_error)
{
	std::function<int()> function;
	bool caught = false;

	try {
		(void)function();
	} catch (const std::bad_function_call &) {
		caught = true;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_allocation_failure_throws)
{
	volatile size_t requested = std::numeric_limits<size_t>::max();
	bool caught = false;

	try {
		void *allocation = ::operator new(requested);

		::operator delete(allocation);
	} catch (const std::bad_alloc &) {
		caught = true;
	}
	zassert_true(caught);
}

ZTEST(libcxx_exceptions, test_filesystem_errors_preserve_paths)
{
	namespace fs = std::filesystem;
	const fs::path source("/libcxx-missing/source");
	const fs::path destination("/libcxx-missing/destination");
	bool missing_caught = false;
	bool unsupported_caught = false;
	bool attributes_caught = false;
	const fs::path attributes("/");

	try {
		(void)fs::file_size(source);
	} catch (const fs::filesystem_error &error) {
		missing_caught = error.code() == std::errc::no_such_file_or_directory &&
				 error.path1() == source && error.path2().empty();
	}
	try {
		(void)fs::copy_file(source, destination, fs::copy_options::update_existing);
	} catch (const fs::filesystem_error &error) {
		unsupported_caught = error.code() == std::errc::not_supported &&
				     error.path1() == source && error.path2() == destination;
	}
	try {
		(void)fs::create_directory(source, attributes);
	} catch (const fs::filesystem_error &error) {
		attributes_caught = error.code() == std::errc::no_such_file_or_directory &&
				    error.path1() == source && error.path2() == attributes;
	}
	zassert_true(missing_caught);
	zassert_true(unsupported_caught);
	zassert_true(attributes_caught);
}

ZTEST_SUITE(libcxx_exceptions, nullptr, nullptr, nullptr, nullptr, nullptr);
