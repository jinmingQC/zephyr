/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LIB_CPP_LIBCXX_DIRECTORY_SUPPORT_H_
#define ZEPHYR_LIB_CPP_LIBCXX_DIRECTORY_SUPPORT_H_

#include <dirent.h>
#include <errno.h>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

extern "C" DIR *z_libcxx_opendir(const char *name);
extern "C" struct dirent *z_libcxx_readdir(DIR *directory);
extern "C" int z_libcxx_closedir(DIR *directory);

_LIBCPP_BEGIN_NAMESPACE_FILESYSTEM

namespace detail
{

/*
 * The generated upstream iterator uses this small native helper in place of
 * file_descriptor.h. No fabricated POSIX stat metadata is needed for iteration.
 */
inline std::pair<std::string_view, file_type> posix_readdir(DIR *directory, error_code &ec)
{
	errno = 0;
	ec.clear();

	struct dirent *entry = z_libcxx_readdir(directory);

	if (entry == nullptr) {
		if (errno != 0) {
			ec = error_code(errno, generic_category());
		}
		return {};
	}

	/* Query metadata through the native filesystem backend when it is needed. */
	return {entry->d_name, file_type::none};
}

} /* namespace detail */

_LIBCPP_END_NAMESPACE_FILESYSTEM

#endif /* ZEPHYR_LIB_CPP_LIBCXX_DIRECTORY_SUPPORT_H_ */
