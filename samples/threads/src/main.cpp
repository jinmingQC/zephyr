/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

int main()
{
	std::mutex mutex;
	std::condition_variable ready;
	bool start = false;
	uint32_t count = 0;
	std::array<std::thread, 4> workers;

	for (auto &worker : workers) {
		worker = std::thread([&]() {
			{
				std::unique_lock<std::mutex> lock(mutex);

				ready.wait(lock, [&]() { return start; });
			}
			for (uint32_t i = 0; i < 1000; ++i) {
				std::lock_guard<std::mutex> lock(mutex);

				++count;
			}
		});
	}

	{
		std::lock_guard<std::mutex> lock(mutex);

		start = true;
	}
	ready.notify_all();
	for (auto &worker : workers) {
		worker.join();
	}
	std::printf("libc++ threads: %u increments\n", static_cast<unsigned int>(count));
	return count == 4000 ? 0 : 1;
}
