/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LIB_CPP_LIBCXX_FILESYSTEM_SUPPORT_H_
#define ZEPHYR_LIB_CPP_LIBCXX_FILESYSTEM_SUPPORT_H_

#include <filesystem>

namespace zephyr_libcxx
{

/*
 * Resolve against the libc++ current directory and validate each component.
 * Return a positive errno value on failure. allow_missing permits a missing
 * final component for creation and rename operations.
 */
int resolve(const std::filesystem::path &input, std::filesystem::path &absolute,
	    bool allow_missing = false);

/* Compare LittleFS object identities; inputs have already been resolved. */
int identity_equal(const std::filesystem::path &left, const std::filesystem::path &right,
		   bool &same);

/* Identify a mounted volume root or the virtual filesystem root. */
bool mount_root(const std::filesystem::path &absolute);

} /* namespace zephyr_libcxx */

#endif /* ZEPHYR_LIB_CPP_LIBCXX_FILESYSTEM_SUPPORT_H_ */
