/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "__zephyr_stdio.h"
#include "filesystem_support.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <new>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <zephyr/fs/fs.h>
#pragma GCC diagnostic pop

namespace
{

struct native_stream {
	struct fs_file_t file;
};

ssize_t stream_read(void *cookie, void *buffer, size_t size)
{
	auto *stream = static_cast<native_stream *>(cookie);
	const ssize_t result = fs_read(&stream->file, buffer, size);

	if (result < 0) {
		errno = static_cast<int>(-result);
		return -1;
	}
	return result;
}

ssize_t stream_write(void *cookie, const void *buffer, size_t size)
{
	auto *stream = static_cast<native_stream *>(cookie);
	const ssize_t result = fs_write(&stream->file, buffer, size);

	if (result < 0) {
		errno = static_cast<int>(-result);
		return -1;
	}
	if ((size != 0 && result == 0) || static_cast<size_t>(result) > size) {
		errno = EIO;
		return -1;
	}
	/*
	 * funopen has no separate sync callback. Commit each buffered write so
	 * ostream::flush() makes contents visible through other file handles.
	 */
	const int ret = fs_sync(&stream->file);

	if (ret != 0) {
		errno = -ret;
		return -1;
	}
	return result;
}

off_t stream_seek(void *cookie, off_t offset, int origin)
{
	auto *stream = static_cast<native_stream *>(cookie);
	int native_origin;

	switch (origin) {
	case SEEK_SET:
		native_origin = FS_SEEK_SET;
		break;
	case SEEK_CUR:
		native_origin = FS_SEEK_CUR;
		break;
	case SEEK_END:
		native_origin = FS_SEEK_END;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	int ret = fs_seek(&stream->file, offset, native_origin);

	if (ret != 0) {
		errno = -ret;
		return -1;
	}
	const off_t position = fs_tell(&stream->file);

	if (position < 0) {
		errno = static_cast<int>(-position);
		return -1;
	}
	return position;
}

int stream_close(void *cookie)
{
	std::unique_ptr<native_stream> stream(static_cast<native_stream *>(cookie));
	const int ret = fs_close(&stream->file);

	if (ret != 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

} /* namespace */

extern "C" FILE *z_libcxx_fopen(const char *name, const char *mode)
{
	if (name == nullptr || mode == nullptr || mode[0] == '\0') {
		errno = EINVAL;
		return nullptr;
	}
	fs_mode_t flags;

	switch (mode[0]) {
	case 'r':
		flags = FS_O_READ;
		break;
	case 'w':
		flags = FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC;
		break;
	case 'a':
		flags = FS_O_CREATE | FS_O_WRITE | FS_O_APPEND;
		break;
	default:
		errno = EINVAL;
		return nullptr;
	}
	bool update = false;
	bool binary = false;

	for (const char *part = mode + 1; *part != '\0'; ++part) {
		if (*part == '+' && !update) {
			update = true;
			flags |= FS_O_RDWR;
		} else if (*part == 'b' && !binary) {
			binary = true;
		} else {
			errno = EINVAL;
			return nullptr;
		}
	}
	std::filesystem::path absolute;
	int ret = zephyr_libcxx::resolve(name, absolute, (flags & FS_O_CREATE) != 0);

	if (ret != 0) {
		errno = ret;
		return nullptr;
	}
	if (name[0] != '\0' && name[std::strlen(name) - 1] == '/') {
		struct fs_dirent entry;

		ret = fs_stat(absolute.c_str(), &entry);
		errno = ret != 0 ? -ret : EISDIR;
		return nullptr;
	}
	std::unique_ptr<native_stream> stream(new (std::nothrow) native_stream{});

	if (stream == nullptr) {
		errno = ENOMEM;
		return nullptr;
	}
	fs_file_t_init(&stream->file);
	ret = fs_open(&stream->file, absolute.c_str(), flags);
	if (ret != 0) {
		errno = -ret;
		return nullptr;
	}
	FILE *file = funopen(stream.get(), (flags & FS_O_READ) != 0 ? stream_read : nullptr,
			     (flags & FS_O_WRITE) != 0 ? stream_write : nullptr, stream_seek,
			     stream_close);

	if (file == nullptr) {
		const int saved_errno = errno;

		(void)fs_close(&stream->file);
		errno = saved_errno != 0 ? saved_errno : ENOMEM;
		return nullptr;
	}
	(void)stream.release();
	return file;
}
