/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "error.h"
#include "filesystem_support.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <zephyr/fs/fs.h>
#pragma GCC diagnostic pop

namespace fs = std::filesystem;

namespace
{

std::mutex copy_lock;

bool option_set(fs::copy_options options, fs::copy_options flag)
{
	return (options & flag) != fs::copy_options::none;
}

int validate_options(fs::copy_options options)
{
	const auto existing = static_cast<uint16_t>(
		options & (fs::copy_options::skip_existing | fs::copy_options::overwrite_existing |
			   fs::copy_options::update_existing));

	if (existing != 0 && (existing & (existing - 1U)) != 0) {
		return EINVAL;
	}
	if (option_set(options, fs::copy_options::update_existing) ||
	    option_set(options, fs::copy_options::create_symlinks) ||
	    option_set(options, fs::copy_options::create_hard_links)) {
		return ENOTSUP;
	}
	return 0;
}

bool trailing_separator(const fs::path &path)
{
	return !path.empty() && path.native().back() == '/';
}

int destination_status(const fs::path &path, struct fs_dirent &entry, bool &exists)
{
	int ret = fs_stat(path.c_str(), &entry);

	exists = ret == 0;
	return ret == -ENOENT ? 0 : -ret;
}

int copy_file_contents(const fs::path &from, const fs::path &to, bool create)
{
	struct fs_file_t input;
	struct fs_file_t output;

	fs_file_t_init(&input);
	fs_file_t_init(&output);

	int ret = fs_open(&input, from.c_str(), FS_O_READ);

	if (ret != 0) {
		return -ret;
	}
	const fs_mode_t flags = FS_O_WRITE | (create ? FS_O_CREATE : 0);

	ret = fs_open(&output, to.c_str(), flags);
	if (ret != 0) {
		const int close_ret = fs_close(&input);

		return ret != 0 ? -ret : -close_ret;
	}

	/* Identity has been checked before either opening or truncating output. */
	ret = fs_truncate(&output, 0);
	uint8_t buffer[512];

	while (ret == 0) {
		const ssize_t count = fs_read(&input, buffer, sizeof(buffer));

		if (count < 0) {
			ret = static_cast<int>(count);
			break;
		}
		if (count == 0) {
			break;
		}
		size_t written = 0;

		while (written < static_cast<size_t>(count)) {
			const size_t remaining = static_cast<size_t>(count) - written;
			const ssize_t result = fs_write(&output, buffer + written, remaining);

			if (result < 0) {
				ret = static_cast<int>(result);
				break;
			}
			if (result == 0 || static_cast<size_t>(result) > remaining) {
				ret = -EIO;
				break;
			}
			written += static_cast<size_t>(result);
		}
	}

	const int output_close = fs_close(&output);
	const int input_close = fs_close(&input);

	if (ret == 0) {
		ret = output_close != 0 ? output_close : input_close;
	}
	return -ret;
}

int copy_file_checked(const fs::path &from, const fs::path &to, fs::copy_options options,
		      bool &copied)
{
	copied = false;

	int ret = validate_options(options);
	fs::path source;
	fs::path destination;

	if (ret == 0) {
		ret = zephyr_libcxx::resolve(from, source);
	}
	if (ret == 0) {
		ret = zephyr_libcxx::resolve(to, destination, true);
	}
	if (ret != 0) {
		return ret;
	}

	struct fs_dirent source_entry;
	struct fs_dirent target_entry;
	bool target_exists;

	ret = fs_stat(source.c_str(), &source_entry);
	if (ret != 0) {
		return -ret;
	}
	if (source_entry.type != FS_DIR_ENTRY_FILE) {
		return ENOTSUP;
	}
	ret = destination_status(destination, target_entry, target_exists);
	if (ret != 0) {
		return ret;
	}
	if (target_exists && target_entry.type != FS_DIR_ENTRY_FILE) {
		return EISDIR;
	}
	if (!target_exists && trailing_separator(to)) {
		return ENOENT;
	}

	bool same = false;

	ret = zephyr_libcxx::identity_equal(source, destination, same);
	if (ret != 0) {
		return ret;
	}
	if (same) {
		return EEXIST;
	}
	if (target_exists) {
		if (option_set(options, fs::copy_options::skip_existing)) {
			return 0;
		}
		if (!option_set(options, fs::copy_options::overwrite_existing)) {
			return EEXIST;
		}
	}

	ret = copy_file_contents(source, destination, !target_exists);
	copied = ret == 0;
	return ret;
}

int read_children(const fs::path &directory, std::vector<fs::path> &children)
{
	struct fs_dir_t stream;

	fs_dir_t_init(&stream);
	int ret = fs_opendir(&stream, directory.c_str());

	if (ret != 0) {
		return -ret;
	}
	/* Path/vector allocation can throw while the directory is open. */
	auto close_directory = [](struct fs_dir_t *handle) { (void)fs_closedir(handle); };
	std::unique_ptr<struct fs_dir_t, decltype(close_directory)> cleanup(&stream,
									    close_directory);

	for (;;) {
		struct fs_dirent entry;

		ret = fs_readdir(&stream, &entry);
		if (ret != 0 || entry.name[0] == '\0') {
			break;
		}
		if (std::memchr(entry.name, '\0', sizeof(entry.name)) == nullptr) {
			ret = -EIO;
			break;
		}
		if (std::strcmp(entry.name, ".") == 0 || std::strcmp(entry.name, "..") == 0) {
			continue;
		}
		children.emplace_back(directory / entry.name);
	}
	const int close_ret = fs_closedir(cleanup.release());

	return ret != 0 ? -ret : -close_ret;
}

int reject_descendant(const fs::path &source, fs::path destination)
{
	/* Compare mount-relative identity, including aliases of the same volume. */
	while (destination != destination.root_path() && !destination.empty()) {
		bool same = false;
		int ret = zephyr_libcxx::identity_equal(source, destination, same);

		if (ret != 0) {
			return ret;
		}
		if (same) {
			return EINVAL;
		}
		if (zephyr_libcxx::mount_root(destination)) {
			break;
		}
		destination = destination.parent_path();
	}
	return 0;
}

struct copy_job {
	fs::path source;
	fs::path destination;
	bool root;
};

int copy_tree(const fs::path &from, const fs::path &to, fs::copy_options options)
{
	int ret = validate_options(options);

	if (ret != 0) {
		return ret;
	}

	std::vector<copy_job> pending;
	pending.push_back({from, to, true});

	while (!pending.empty()) {
		copy_job job = std::move(pending.back());

		pending.pop_back();

		fs::path source;
		fs::path destination;
		struct fs_dirent source_entry;
		struct fs_dirent target_entry;
		bool target_exists;

		ret = zephyr_libcxx::resolve(job.source, source);
		if (ret == 0) {
			ret = zephyr_libcxx::resolve(job.destination, destination, true);
		}
		if (ret != 0) {
			return ret;
		}
		ret = fs_stat(source.c_str(), &source_entry);
		if (ret != 0) {
			return -ret;
		}
		ret = destination_status(destination, target_entry, target_exists);
		if (ret != 0) {
			return ret;
		}

		bool same = false;

		ret = zephyr_libcxx::identity_equal(source, destination, same);
		if (ret != 0) {
			return ret;
		}
		if (same) {
			return EEXIST;
		}

		if (source_entry.type == FS_DIR_ENTRY_FILE) {
			if (option_set(options, fs::copy_options::directories_only)) {
				continue;
			}
			if (target_exists && target_entry.type == FS_DIR_ENTRY_DIR) {
				destination /= source.filename();
			} else if (!target_exists && trailing_separator(job.destination)) {
				return ENOENT;
			}

			bool copied;

			ret = copy_file_checked(source, destination, options, copied);
			if (ret != 0) {
				return ret;
			}
			continue;
		}
		if (source_entry.type != FS_DIR_ENTRY_DIR) {
			return ENOTSUP;
		}
		if (target_exists && target_entry.type != FS_DIR_ENTRY_DIR) {
			return ENOTDIR;
		}

		const bool recursive = option_set(options, fs::copy_options::recursive);

		if (!recursive && (!job.root || options != fs::copy_options::none)) {
			continue;
		}

		/* Reject recursive self-copy before creating even the destination root. */
		ret = reject_descendant(source, destination);
		if (ret != 0) {
			return ret;
		}
		if (!target_exists) {
			std::error_code error;

			(void)fs::__create_directory(destination, &error);
			if (error) {
				return error.value();
			}
		}

		std::vector<fs::path> children;

		ret = read_children(source, children);
		if (ret != 0) {
			return ret;
		}
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			pending.push_back({*child, destination / child->filename(), false});
		}
	}
	return 0;
}

struct remove_job {
	fs::path name;
	bool visited;
};

int remove_tree(const fs::path &input, uintmax_t &removed)
{
	/* A final dot component must not turn into removal of its resolved parent. */
	auto component = input.end();

	while (component != input.begin()) {
		--component;
		if (!component->empty()) {
			if (*component == "." || *component == "..") {
				return EINVAL;
			}
			break;
		}
	}

	fs::path absolute;
	int ret = zephyr_libcxx::resolve(input, absolute, true);

	removed = 0;
	if (ret == ENOENT) {
		return 0;
	}
	if (ret != 0) {
		return ret;
	}
	if (zephyr_libcxx::mount_root(absolute)) {
		return EBUSY;
	}

	std::vector<remove_job> pending;
	pending.push_back({absolute, false});

	while (!pending.empty()) {
		remove_job job = std::move(pending.back());

		pending.pop_back();
		if (zephyr_libcxx::mount_root(job.name)) {
			return EBUSY;
		}
		if (!job.visited) {
			struct fs_dirent entry;

			ret = fs_stat(job.name.c_str(), &entry);
			if (ret == -ENOENT) {
				continue;
			}
			if (ret != 0) {
				return -ret;
			}
			if (entry.type == FS_DIR_ENTRY_DIR) {
				std::vector<fs::path> children;

				ret = read_children(job.name, children);
				if (ret != 0) {
					return ret;
				}
				pending.push_back({job.name, true});
				for (auto child = children.rbegin(); child != children.rend();
				     ++child) {
					pending.push_back({*child, false});
				}
				continue;
			}
		}

		std::error_code error;
		const bool did_remove = fs::__remove(job.name, &error);

		if (error) {
			return error.value();
		}
		if (did_remove) {
			++removed;
		}
	}
	return 0;
}

} /* namespace */

_LIBCPP_BEGIN_NAMESPACE_FILESYSTEM

bool __copy_file(const path &from, const path &to, copy_options options, error_code *ec)
{
	detail::ErrorHandler<bool> error("copy_file", ec, &from, &to);
	std::lock_guard<std::mutex> lock(copy_lock);
	bool copied;
	int ret = copy_file_checked(from, to, options, copied);

	return ret == 0 ? copied : error.report(error_code(ret, generic_category()));
}

void __copy(const path &from, const path &to, copy_options options, error_code *ec)
{
	detail::ErrorHandler<void> error("copy", ec, &from, &to);
	std::lock_guard<std::mutex> lock(copy_lock);
	int ret = copy_tree(from, to, options);

	if (ret != 0) {
		error.report(error_code(ret, generic_category()));
	}
}

uintmax_t __remove_all(const path &input, error_code *ec)
{
	detail::ErrorHandler<uintmax_t> error("remove_all", ec, &input);
	std::lock_guard<std::mutex> lock(copy_lock);
	uintmax_t removed;
	int ret = remove_tree(input, removed);

	return ret == 0 ? removed : error.report(error_code(ret, generic_category()));
}

_LIBCPP_END_NAMESPACE_FILESYSTEM
