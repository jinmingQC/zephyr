/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <__system_error/system_error.h>
#include <cerrno>
#include <random>
#include <zephyr/random/random.h>

_LIBCPP_BEGIN_NAMESPACE_STD

random_device::random_device(const string &token)
#ifdef _LIBCPP_USING_DEV_RANDOM
	: __f_(-1)
#endif
{
	if (token != "/dev/urandom") {
		__throw_system_error(ENOENT, "Unsupported random_device token");
	}
}

random_device::~random_device() = default;

unsigned random_device::operator()()
{
	unsigned result;
	const int ret = sys_csrand_get(&result, sizeof(result));

	if (ret != 0) {
		__throw_system_error(ret < 0 ? -ret : EIO, "Native random source failed");
	}
	return result;
}

double random_device::entropy() const noexcept
{
	/* The native API does not report a measured entropy estimate. */
	return 0.0;
}

_LIBCPP_END_NAMESPACE_STD
