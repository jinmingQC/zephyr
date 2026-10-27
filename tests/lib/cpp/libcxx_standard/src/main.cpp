/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bitset>
#include <charconv>
#include <cstdint>
#include <deque>
#include <forward_list>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

BUILD_ASSERT(__cplusplus == 201703L);

/* Force the out-of-line compiler ABI entry even on targets with native 64-bit
 * atomics. */
extern "C" uint64_t z_test_atomic_fetch_add_8(volatile void *object, uint64_t value,
					      int order) __asm__("__atomic_fetch_add_8");

namespace
{

struct category_probe: std::error_category {
	const char *name() const noexcept override
	{
		return "category probe";
	}

	std::string message(int) const override
	{
		return "category probe";
	}
};

struct aligned_value {
	alignas(64) std::array<uint8_t, 64> bytes{};
};

struct shared_value: std::enable_shared_from_this<shared_value> {
	int value = 71;
};

struct atomic_words {
	uint32_t a;
	uint32_t b;
	uint32_t c;
};

BUILD_ASSERT(sizeof(atomic_words) == 3U * sizeof(uint32_t));
BUILD_ASSERT(std::is_trivially_copyable_v<atomic_words>);
BUILD_ASSERT(sizeof(std::atomic<atomic_words>) == sizeof(atomic_words));
BUILD_ASSERT(alignof(std::atomic<atomic_words>) == alignof(atomic_words));

struct operation {
	int base = 9;

	int add(int value) const
	{
		return base + value;
	}
};

} /* namespace */

ZTEST(libcxx_standard, test_sequence_containers_and_algorithms)
{
	std::vector<int> values{5, 1, 4, 2, 3};

	std::sort(values.begin(), values.end());
	zassert_true((values == std::vector<int>{1, 2, 3, 4, 5}));
	std::list<int> linked(values.begin(), values.end());

	linked.remove_if([](int value) { return value % 2 == 0; });
	linked.reverse();
	zassert_true((std::vector<int>(linked.begin(), linked.end()) == std::vector<int>{5, 3, 1}));
	std::deque<int> queue{2, 3};

	queue.push_front(1);
	queue.push_back(4);
	zassert_equal(queue.front(), 1);
	zassert_equal(queue.back(), 4);

	/* Cross multiple deque blocks and map reallocations in both directions. */
	for (int value = 0; value < 1024; ++value) {
		queue.push_front(-value);
		queue.push_back(1000 + value);
	}
	zassert_equal(queue.size(), 2052);
	for (int value = 1023; value >= 0; --value) {
		zassert_equal(queue.front(), -value);
		queue.pop_front();
		zassert_equal(queue.back(), 1000 + value);
		queue.pop_back();
	}
	zassert_equal(queue.size(), 4);
	zassert_equal(queue.front(), 1);
	zassert_equal(queue.back(), 4);
	std::forward_list<int> forward{3, 1, 2};

	forward.sort();
	zassert_true(
		(std::vector<int>(forward.begin(), forward.end()) == std::vector<int>{1, 2, 3}));
}

ZTEST(libcxx_standard, test_associative_containers_and_node_handles)
{
	std::map<std::string, int> values;

	auto [first, inserted] = values.try_emplace("one", 1);

	zassert_true(inserted);
	zassert_equal(first->second, 1);
	auto [existing, replaced] = values.insert_or_assign("one", 11);

	zassert_false(replaced);
	zassert_equal(existing->second, 11);
	auto node = values.extract("one");

	zassert_false(node.empty());
	node.key() = "eleven";
	auto result = values.insert(std::move(node));

	zassert_true(result.inserted);
	zassert_equal(values.count("one"), 0);
	zassert_equal(values.at("eleven"), 11);

	std::unordered_map<std::string, int> hashed{{"a", 3}, {"b", 5}};

	hashed.reserve(32);
	zassert_equal(hashed.find("a")->second + hashed.find("b")->second, 8);
	std::set<int> unique{3, 1, 3, 2};

	zassert_equal(unique.size(), 3);
	zassert_equal(*unique.begin(), 1);
}

ZTEST(libcxx_standard, test_cpp17_numeric_algorithms)
{
	const std::array<int, 4> values{1, 2, 3, 4};
	std::array<int, 4> scan{};

	zassert_equal(std::reduce(values.begin(), values.end(), 0), 10);
	std::exclusive_scan(values.begin(), values.end(), scan.begin(), 0);
	zassert_true((scan == std::array<int, 4>{0, 1, 3, 6}));
	zassert_equal(std::transform_reduce(values.begin(), values.end(), values.begin(), 0), 30);
	zassert_equal(std::clamp(12, 3, 9), 9);
	zassert_equal(std::gcd(54, 24), 6);
	zassert_equal(std::lcm(6, 8), 24);
}

ZTEST(libcxx_standard, test_string_view_and_embedded_null)
{
	const std::string text("ab\0cd", 5);
	std::string_view view(text.data(), text.size());

	zassert_equal(view.size(), 5);
	zassert_equal(view.find('\0'), 2);
	zassert_true(view.substr(3) == "cd");
	view.remove_prefix(1);
	view.remove_suffix(1);
	zassert_equal(view.size(), 3);
	zassert_equal(view[0], 'b');
	zassert_equal(view[1], '\0');
	zassert_true(std::string(view) == std::string("b\0c", 3));
}

ZTEST(libcxx_standard, test_optional_variant_and_any)
{
	std::optional<std::string> optional;

	zassert_false(optional.has_value());
	zassert_true(optional.value_or("fallback") == "fallback");
	optional.emplace("value");
	zassert_true(*optional == "value");
	optional.reset();
	zassert_false(optional.has_value());

	std::variant<int, std::string> variant(12);

	zassert_true(std::get_if<int>(&variant) != nullptr);
	zassert_equal(std::get<int>(variant), 12);
	variant = std::string("payload");
	const size_t length = std::visit(
		[](const auto &value) -> size_t {
			if constexpr (std::is_same_v<std::decay_t<decltype(value)>, int>) {
				return static_cast<size_t>(value);
			} else {
				return value.size();
			}
		},
		variant);

	zassert_equal(length, 7);
	std::any any(std::vector<int>{2, 4, 6});
	auto moved = std::move(any);
	const auto *contents = std::any_cast<std::vector<int>>(&moved);

	zassert_not_null(contents);
	zassert_equal((*contents)[2], 6);
	zassert_true(std::any_cast<std::string>(&moved) == nullptr);
	moved.reset();
	zassert_false(moved.has_value());
}

ZTEST(libcxx_standard, test_apply_invoke_and_function)
{
	operation object;
	auto arguments = std::make_tuple(3, 4);

	zassert_equal(std::apply([](int left, int right) { return left * right; }, arguments), 12);
	zassert_equal(std::invoke(&operation::add, object, 5), 14);
	zassert_equal(std::invoke(&operation::base, object), 9);
	std::function<int(int)> function =
		std::bind(&operation::add, std::cref(object), std::placeholders::_1);

	zassert_equal(function(8), 17);
#if defined(CONFIG_CPP_EXCEPTIONS)
	category_probe category;
	std::function<void()> throwing = [&category] { throw &category; };
	bool matched = false;

	try {
		std::invoke(throwing);
	} catch (const std::error_category *caught) {
		matched = caught == &category;
	}
	zassert_true(matched);
#endif
}

ZTEST(libcxx_standard, test_smart_pointer_ownership_and_aliasing)
{
	auto owner = std::make_shared<shared_value>();
	std::weak_ptr<shared_value> weak(owner);
	std::shared_ptr<int> alias(owner, &owner->value);

	zassert_true(owner->shared_from_this() == owner);
	zassert_equal(*alias, 71);
	owner.reset();
	zassert_false(weak.expired());
	zassert_equal(weak.lock()->value, 71);
	alias.reset();
	zassert_true(weak.expired());

	int deleted = 0;

	{
		auto cleanup = [&deleted](int *value) {
			++deleted;
			delete value;
		};
		std::unique_ptr<int, decltype(cleanup)> value(new int(13), cleanup);

		zassert_equal(*value, 13);
	}
	zassert_equal(deleted, 1);
}

ZTEST(libcxx_standard, test_integer_charconv)
{
	std::array<char, 64> buffer{};
	constexpr int64_t expected = -1234567890123456789LL;
	const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), expected);

	zassert_true(written.ec == std::errc{});
	int64_t decoded = 0;
	const auto parsed = std::from_chars(buffer.data(), written.ptr, decoded);

	zassert_true(parsed.ec == std::errc{});
	zassert_true(parsed.ptr == written.ptr);
	zassert_equal(decoded, expected);

	uint32_t hex = 0;
	const std::string_view text("deadbeef");
	const auto parsed_hex = std::from_chars(text.data(), text.data() + text.size(), hex, 16);

	zassert_true(parsed_hex.ec == std::errc{});
	zassert_equal(hex, 0xdeadbeefU);
	const auto short_buffer = std::to_chars(buffer.data(), buffer.data() + 1, 12345);

	zassert_true(short_buffer.ec == std::errc::value_too_large);
}

ZTEST(libcxx_standard, test_floating_to_chars)
{
	std::array<char, 64> buffer{};
	const std::array<double, 4> values{0.0, -0.125, 1.5, 12345.75};
	const std::array<std::string_view, 4> expected{"0", "-0.125", "1.5", "12345.75"};

	for (size_t index = 0; index < values.size(); ++index) {
		const auto written =
			std::to_chars(buffer.data(), buffer.data() + buffer.size(), values[index]);

		zassert_true(written.ec == std::errc{});
		const std::string_view output(buffer.data(), written.ptr - buffer.data());

		zassert_true(output == expected[index]);
	}
}

ZTEST(libcxx_standard, test_polymorphic_memory_resources)
{
	alignas(64) std::array<std::byte, 1024> arena{};
	std::pmr::monotonic_buffer_resource resource(arena.data(), arena.size(),
						     std::pmr::null_memory_resource());

	{
		std::pmr::vector<uint32_t> values(&resource);

		values.reserve(16);
		for (uint32_t value = 0; value < 16; ++value) {
			values.push_back(value * value);
		}
		const auto address = reinterpret_cast<uintptr_t>(values.data());
		const auto first = reinterpret_cast<uintptr_t>(arena.data());

		zassert_true(address >= first);
		zassert_true(address + values.size() * sizeof(uint32_t) <= first + arena.size());
		zassert_equal(values.back(), 225);
	}
	resource.release();

	std::pmr::unsynchronized_pool_resource pool;
	void *block = pool.allocate(128, 64);

	zassert_equal(reinterpret_cast<uintptr_t>(block) % 64, 0);
	std::fill_n(static_cast<uint8_t *>(block), 128, 0x5a);
	pool.deallocate(block, 128, 64);
}

ZTEST(libcxx_standard, test_overaligned_allocation)
{
	auto value = std::make_unique<aligned_value>();

	zassert_equal(reinterpret_cast<uintptr_t>(value.get()) % alignof(aligned_value), 0);
	value->bytes.fill(0x7b);
	zassert_equal(value->bytes.front(), 0x7b);
	zassert_equal(value->bytes.back(), 0x7b);
}

ZTEST(libcxx_standard, test_64_bit_atomic_contention)
{
	constexpr uint64_t initial = 0x100000000ULL;
	constexpr uint64_t increment = 0x100000001ULL;
	std::atomic<uint64_t> value{initial};
	std::array<std::thread, 2> workers;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			for (int iteration = 0; iteration < 64; ++iteration) {
				value.fetch_add(increment, std::memory_order_relaxed);
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	uint64_t expected = initial + 128U * increment;

	zassert_equal(value.load(std::memory_order_acquire), expected);
	zassert_true(value.compare_exchange_strong(expected, 7, std::memory_order_acq_rel));
	zassert_equal(value.load(), 7);
	zassert_equal(value.exchange(initial), 7);
	zassert_equal(value.load(), initial);
}

ZTEST(libcxx_standard, test_64_bit_atomic_operations)
{
	std::atomic<uint64_t> value{0xffffffffULL};

	zassert_equal(value.fetch_add(1), 0xffffffffULL);
	zassert_equal(value.load(), 0x100000000ULL);
	zassert_equal(value.fetch_sub(1), 0x100000000ULL);
	zassert_equal(value.load(), 0xffffffffULL);

	value.store(0x0123456789abcdefULL, std::memory_order_release);
	zassert_equal(value.fetch_and(0x00ff00ffff00ff00ULL), 0x0123456789abcdefULL);
	zassert_equal(value.load(), 0x002300678900cd00ULL);
	zassert_equal(value.fetch_or(0x8000000000000080ULL), 0x002300678900cd00ULL);
	zassert_equal(value.load(), 0x802300678900cd80ULL);
	zassert_equal(value.fetch_xor(0xff00ff0000ff00ffULL), 0x802300678900cd80ULL);
	zassert_equal(value.load(), 0x7f23ff6789ffcd7fULL);

	uint64_t expected = 0x7f23ff6789ffcd7fULL;

	zassert_true(value.compare_exchange_strong(expected, 0x0fedcba987654321ULL));
	zassert_equal(expected, 0x7f23ff6789ffcd7fULL);
	zassert_equal(value.load(), 0x0fedcba987654321ULL);

	expected = 0x1111222233334444ULL;
	zassert_false(value.compare_exchange_strong(expected, 0x777788889999aaaaULL));
	zassert_equal(expected, 0x0fedcba987654321ULL);
	zassert_equal(value.load(), 0x0fedcba987654321ULL);
}

ZTEST(libcxx_standard, test_64_bit_atomic_load_store_do_not_tear)
{
	std::atomic<uint64_t> value{0xffffffff00000000ULL};
	std::atomic<bool> start{false};
	std::atomic<uint32_t> failures{0};
	std::thread writer([&] {
		while (!start.load()) {
			std::this_thread::yield();
		}
		for (uint32_t sample = 1; sample <= 512; ++sample) {
			const uint64_t pattern = (static_cast<uint64_t>(~sample) << 32) | sample;

			value.store(pattern, std::memory_order_release);
			std::this_thread::yield();
		}
	});
	std::thread reader([&] {
		while (!start.load()) {
			std::this_thread::yield();
		}
		for (uint32_t iteration = 0; iteration < 1024; ++iteration) {
			const uint64_t sample = value.load(std::memory_order_acquire);
			const uint32_t low = static_cast<uint32_t>(sample);
			const uint32_t high = static_cast<uint32_t>(sample >> 32);

			if (high != ~low) {
				failures.fetch_add(1);
			}
			std::this_thread::yield();
		}
	});

	start.store(true);
	writer.join();
	reader.join();
	zassert_equal(failures.load(), 0);
	zassert_equal(value.load(), (static_cast<uint64_t>(~uint32_t{512}) << 32) | 512U);
}

ZTEST(libcxx_standard, test_generic_atomic_operations)
{
	std::atomic<atomic_words> value{atomic_words{1, 2, 3}};

	zassert_false(value.is_lock_free());
	auto observed = value.load(std::memory_order_acquire);

	zassert_equal(observed.a, 1);
	zassert_equal(observed.b, 2);
	zassert_equal(observed.c, 3);
	value.store(atomic_words{4, 5, 6}, std::memory_order_release);
	observed = value.exchange(atomic_words{7, 8, 9}, std::memory_order_acq_rel);
	zassert_equal(observed.a, 4);
	zassert_equal(observed.b, 5);
	zassert_equal(observed.c, 6);

	atomic_words expected{7, 8, 9};

	zassert_true(value.compare_exchange_strong(expected, atomic_words{10, 11, 12}));
	zassert_equal(expected.a, 7);
	zassert_equal(expected.b, 8);
	zassert_equal(expected.c, 9);
	observed = value.load();
	zassert_equal(observed.a, 10);
	zassert_equal(observed.b, 11);
	zassert_equal(observed.c, 12);

	expected = atomic_words{1, 2, 3};
	zassert_false(value.compare_exchange_strong(expected, atomic_words{13, 14, 15}));
	zassert_equal(expected.a, 10);
	zassert_equal(expected.b, 11);
	zassert_equal(expected.c, 12);
	observed = value.load();
	zassert_equal(observed.a, 10);
	zassert_equal(observed.b, 11);
	zassert_equal(observed.c, 12);

	expected = atomic_words{1, 2, 3};
	zassert_false(value.compare_exchange_weak(expected, atomic_words{13, 14, 15}));
	zassert_equal(expected.a, 10);
	zassert_equal(expected.b, 11);
	zassert_equal(expected.c, 12);

	/* A weak CAS may fail spuriously; retry while checking its expected update. */
	while (!value.compare_exchange_weak(expected, atomic_words{13, 14, 15})) {
		zassert_equal(expected.a, 10);
		zassert_equal(expected.b, 11);
		zassert_equal(expected.c, 12);
	}
	observed = value.load();
	zassert_equal(observed.a, 13);
	zassert_equal(observed.b, 14);
	zassert_equal(observed.c, 15);
}

ZTEST(libcxx_standard, test_generic_atomic_compare_exchange_contention)
{
	constexpr uint32_t iterations = 128;
	std::atomic<atomic_words> value{atomic_words{0, 0, 0}};
	std::atomic<bool> start{false};
	std::atomic<uint32_t> failures{0};
	std::array<std::thread, 4> workers;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			while (!start.load()) {
				std::this_thread::yield();
			}
			for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
				atomic_words expected = value.load(std::memory_order_acquire);

				for (;;) {
					if (expected.a != expected.b || expected.b != expected.c) {
						failures.fetch_add(1);
					}
					const uint32_t next = expected.a + 1;
					const atomic_words desired{next, next, next};

					if (value.compare_exchange_weak(
						    expected, desired, std::memory_order_acq_rel,
						    std::memory_order_acquire)) {
						break;
					}
					std::this_thread::yield();
				}
			}
		});
	}
	start.store(true);
	for (auto &worker : workers) {
		worker.join();
	}
	const uint32_t total = static_cast<uint32_t>(workers.size()) * iterations;
	const atomic_words observed = value.load();

	zassert_equal(failures.load(), 0);
	zassert_equal(observed.a, total);
	zassert_equal(observed.b, total);
	zassert_equal(observed.c, total);
}

ZTEST(libcxx_standard, test_out_of_line_atomic_interoperates_with_compiler)
{
	constexpr uint64_t increment = 0x100000001ULL;
	alignas(8) uint64_t value = 0;
	std::atomic<bool> start{false};
	std::thread native([&] {
		while (!start.load()) {
			std::this_thread::yield();
		}
		for (uint32_t iteration = 0; iteration < 256; ++iteration) {
			if constexpr (__atomic_always_lock_free(8, nullptr)) {
				(void)__atomic_fetch_add(&value, increment, __ATOMIC_SEQ_CST);
			} else {
				(void)z_test_atomic_fetch_add_8(&value, increment,
								__ATOMIC_SEQ_CST);
			}
			std::this_thread::yield();
		}
	});
	std::thread fallback([&] {
		while (!start.load()) {
			std::this_thread::yield();
		}
		for (uint32_t iteration = 0; iteration < 256; ++iteration) {
			(void)z_test_atomic_fetch_add_8(&value, increment, __ATOMIC_SEQ_CST);
			std::this_thread::yield();
		}
	});

	start.store(true);
	native.join();
	fallback.join();
	/* Both writers have joined, so this final observation needs no atomic access.
	 */
	zassert_equal(value, 512U * increment);
}

ZTEST(libcxx_standard, test_random_engine_and_distribution)
{
	std::mt19937 first(1234);
	std::mt19937 second(1234);
	std::uniform_int_distribution<int> distribution(2, 9);

	for (int iteration = 0; iteration < 32; ++iteration) {
		zassert_equal(first(), second());
	}
	for (int iteration = 0; iteration < 64; ++iteration) {
		const int value = distribution(first);

		zassert_true(value >= 2 && value <= 9);
	}
	std::bitset<65> bits;

	bits.set(0);
	bits.set(32);
	bits.set(64);
	zassert_equal(bits.count(), 3);
	zassert_true(bits[64]);
	zassert_equal((bits >> 32).count(), 2);
}

ZTEST_SUITE(libcxx_standard, nullptr, nullptr, nullptr, nullptr, nullptr);
