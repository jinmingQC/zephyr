/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "filesystem_support.hpp"
#include "zephyr_directory_support.hpp"

#include <cstring>
#include <memory>
#include <new>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <zephyr/fs/fs.h>
#pragma GCC diagnostic pop

namespace
{

struct directory_handle {
	struct fs_dir_t directory;
	struct dirent entry;
};

} /* namespace */

extern "C" DIR *z_libcxx_opendir(const char *name)
{
	if (name == nullptr) {
		errno = EINVAL;
		return nullptr;
	}

	std::filesystem::path absolute;
	int ret = zephyr_libcxx::resolve(name, absolute);

	if (ret != 0) {
		errno = ret;
		return nullptr;
	}

	std::unique_ptr<directory_handle> handle(new (std::nothrow) directory_handle{});

	if (handle == nullptr) {
		errno = ENOMEM;
		return nullptr;
	}
	fs_dir_t_init(&handle->directory);
	ret = fs_opendir(&handle->directory, absolute.c_str());
	if (ret < 0) {
		errno = -ret;
		return nullptr;
	}

	return handle.release();
}

extern "C" struct dirent *z_libcxx_readdir(DIR *directory)
{
	if (directory == nullptr) {
		errno = EBADF;
		return nullptr;
	}

	auto *handle = static_cast<directory_handle *>(directory);
	struct fs_dirent native_entry;
	int ret = fs_readdir(&handle->directory, &native_entry);

	if (ret < 0) {
		errno = -ret;
		return nullptr;
	}
	const size_t length = strnlen(native_entry.name, sizeof(native_entry.name));

	if (length == 0) {
		return nullptr;
	}
	if (length == sizeof(native_entry.name)) {
		errno = EIO;
		return nullptr;
	}
	if (length >= sizeof(handle->entry.d_name)) {
		errno = ENAMETOOLONG;
		return nullptr;
	}

	memcpy(handle->entry.d_name, native_entry.name, length + 1);
	/* Iterators consume the name only; native fs_dirent has no inode number. */
	handle->entry.d_ino = 0;
	return &handle->entry;
}

extern "C" int z_libcxx_closedir(DIR *directory)
{
	if (directory == nullptr) {
		errno = EBADF;
		return -1;
	}

	std::unique_ptr<directory_handle> handle(static_cast<directory_handle *>(directory));
	int ret = fs_closedir(&handle->directory);

	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

_LIBCPP_BEGIN_NAMESPACE_FILESYSTEM

error_code directory_entry::__do_refresh() noexcept
{
	__data_.__reset();

	error_code ec;
	file_status status = __status(__p_, &ec);

	if (status_known(status)) {
		/*
		 * Cache only the type. The iterator-style cache forces size,
		 * permissions, link count and time queries through the backend.
		 */
		__data_ = __create_iter_result(status.type());
	}
	return ec;
}

_LIBCPP_END_NAMESPACE_FILESYSTEM
