/* SPDX-License-Identifier: Apache-2.0 */

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>

namespace {

std::atomic<uint32_t> user_constructions{0};
std::atomic<uint32_t> supervisor_constructions{0};

struct guarded_value {
	explicit guarded_value(std::atomic<uint32_t> &constructions)
	{
		constructions.fetch_add(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		value = 42;
	}

	uint32_t value = 0;
};

} /* namespace */

uint32_t user_static_value()
{
	static guarded_value object(user_constructions);

	return object.value;
}

uint32_t supervisor_static_value()
{
	static guarded_value object(supervisor_constructions);

	return object.value;
}

uint32_t user_static_constructions()
{
	return user_constructions.load();
}

uint32_t supervisor_static_constructions()
{
	return supervisor_constructions.load();
}
