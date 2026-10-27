/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LIB_CPP_LIBCXX_UNWIND_MUTEX_HPP_
#define ZEPHYR_LIB_CPP_LIBCXX_UNWIND_MUTEX_HPP_

#include <zephyr/kernel.h>

namespace libunwind
{

/*
 * libunwind's private cache lock needs no C++ runtime objects. Initialization
 * is lazy and serialized so it also works during application constructors.
 * Readers use the same native mutex as writers.
 */
class _LIBUNWIND_HIDDEN RWMutex
{
      public:
	bool lock_shared()
	{
		return lock();
	}

	bool unlock_shared()
	{
		return unlock();
	}

	bool lock()
	{
		static struct k_spinlock initialization_lock;
		k_spinlock_key_t key = k_spin_lock(&initialization_lock);

		if (!initialized) {
			int ret = k_mutex_init(&mutex);

			if (ret != 0) {
				k_spin_unlock(&initialization_lock, key);
				_LIBUNWIND_ABORT("Cannot initialize unwind mutex");
			}
			initialized = true;
		}
		k_spin_unlock(&initialization_lock, key);
		if (k_mutex_lock(&mutex, K_FOREVER) != 0) {
			_LIBUNWIND_ABORT("Cannot lock unwind mutex");
		}
		return true;
	}

	bool unlock()
	{
		if (k_mutex_unlock(&mutex) != 0) {
			_LIBUNWIND_ABORT("Cannot unlock unwind mutex");
		}
		return true;
	}

      private:
	struct k_mutex mutex{};
	bool initialized = false;
};

} /* namespace libunwind */

#endif /* ZEPHYR_LIB_CPP_LIBCXX_UNWIND_MUTEX_HPP_ */
