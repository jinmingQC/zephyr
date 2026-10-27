/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "filesystem_support.hpp"
#include "error.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <zephyr/fs/fs.h>
#pragma GCC diagnostic pop

#include <zephyr/kernel.h>

namespace
{

namespace fs = std::filesystem;

std::mutex cwd_lock;
char current_directory[CONFIG_LIBCXX_FILESYSTEM_PATH_MAX] = "/";

std::error_code native_error(int value)
{
	return {value, std::generic_category()};
}

/*
 * Native path resolution allocates even for public noexcept error-code APIs.
 * Convert only allocation failure; other unexpected exceptions retain their
 * normal behavior, and throwing filesystem overloads report ENOMEM.
 */
template <typename Operation> int allocation_error(Operation operation)
{
#if defined(CONFIG_CPP_EXCEPTIONS)
	try {
		return operation();
	} catch (const std::bad_alloc &) {
		return ENOMEM;
	}
#else
	return operation();
#endif
}

/* Inspect the native string: path iteration can allocate a component copy. */
bool final_dot_component(const fs::path &input)
{
	const auto &name = input.native();
	const size_t end = name.find_last_not_of('/');

	if (end == std::string::npos) {
		return false;
	}
	const size_t separator = name.find_last_of('/', end);
	const size_t begin = separator == std::string::npos ? 0 : separator + 1;
	const size_t length = end - begin + 1;

	return (length == 1 && name[begin] == '.') ||
	       (length == 2 && name[begin] == '.' && name[begin + 1] == '.');
}

int absolute_path(const fs::path &input, fs::path &output)
{
	if (input.empty()) {
		return ENOENT;
	}
	if (input.native().find('\0') != std::string::npos) {
		return EINVAL;
	}
	if (input.native().size() >= sizeof(current_directory)) {
		return ENAMETOOLONG;
	}
	if (input.is_absolute()) {
		output = input;
	} else {
		std::lock_guard<std::mutex> lock(cwd_lock);

		output = fs::path(current_directory) / input;
	}
	return output.native().size() >= sizeof(current_directory) ? ENAMETOOLONG : 0;
}

/*
 * Resolve components before removing "." and "..". Normalizing first would
 * incorrectly accept "missing/../file" or "regular-file/../directory".
 */
int resolve_path(const fs::path &input, fs::path &output, bool allow_missing,
		 bool allow_missing_suffix)
{
	fs::path absolute;
	int ret = absolute_path(input, absolute);

	if (ret != 0) {
		return ret;
	}
	output = "/";
	struct fs_dirent entry{};
	bool missing = false;

	entry.type = FS_DIR_ENTRY_DIR;
	auto part = absolute.begin();

	for (++part; part != absolute.end(); ++part) {
		if (part->native().size() >= sizeof(entry.name)) {
			return ENAMETOOLONG;
		}
		if (missing && allow_missing_suffix) {
			output /= *part;
			continue;
		}
		if (entry.type != FS_DIR_ENTRY_DIR) {
			if (!allow_missing_suffix) {
				return ENOTDIR;
			}
			/* weakly_canonical keeps the suffix after an existing file. */
			missing = true;
			output /= *part;
			continue;
		}
		if (part->empty() || *part == ".") {
			continue;
		}
		if (*part == "..") {
			output = output == "/" ? output : output.parent_path();
			ret = fs_stat(output.c_str(), &entry);
			if (ret != 0) {
				return -ret;
			}
			continue;
		}

		output /= *part;
		ret = fs_stat(output.c_str(), &entry);
		if (ret != 0) {
			if (ret != -ENOENT) {
				return -ret;
			}
			if (allow_missing_suffix) {
				missing = true;
				continue;
			}
			auto next = part;
			bool last = true;

			for (++next; next != absolute.end(); ++next) {
				if (!next->empty()) {
					last = false;
					break;
				}
			}
			if (allow_missing && last) {
				return 0;
			}
			return ENOENT;
		}
	}
	if (missing) {
		output = output.lexically_normal();
	}
	return 0;
}

int stat_path(const fs::path &input, fs::path &absolute, struct fs_dirent &entry)
{
	int ret = zephyr_libcxx::resolve(input, absolute);

	return ret != 0 ? ret : -fs_stat(absolute.c_str(), &entry);
}

/* Find the actual mount; never synthesize inode numbers from path hashes. */
int find_mount_unchecked(const fs::path &absolute, const struct fs_mount_t **mount,
			 std::string &relative)
{
	int index = 0;
	const char *name;
	std::string selected;
	const auto &text = absolute.native();

	while (fs_readmount(&index, &name) == 0) {
		size_t length = strlen(name);

		if (length > selected.size() && text.compare(0, length, name) == 0 &&
		    (text.size() == length || text[length] == '/')) {
			selected = name;
		}
	}
	if (selected.empty()) {
		return ENOTSUP;
	}

	/* Allocate the relative name before acquiring a native directory handle. */
	relative = text.substr(selected.size());
	struct fs_dir_t directory;

	fs_dir_t_init(&directory);
	int ret = fs_opendir(&directory, selected.c_str());

	if (ret != 0) {
		return -ret;
	}
	*mount = directory.mp;
	return -fs_closedir(&directory);
}

int find_mount(const fs::path &absolute, const struct fs_mount_t **mount, std::string &relative)
{
	return allocation_error([&] { return find_mount_unchecked(absolute, mount, relative); });
}

int unsupported_metadata(const fs::path &input)
{
	fs::path absolute;
	int ret = zephyr_libcxx::resolve(input, absolute);

	return ret == 0 ? ENOTSUP : ret;
}

} /* namespace */

namespace zephyr_libcxx
{

int resolve(const std::filesystem::path &input, std::filesystem::path &absolute, bool allow_missing)
{
	return allocation_error(
		[&] { return resolve_path(input, absolute, allow_missing, false); });
}

bool mount_root(const std::filesystem::path &absolute)
{
	const auto &name_string = absolute.native();

	if (name_string == "/") {
		return true;
	}
	int index = 0;
	const char *name;

	while (fs_readmount(&index, &name) == 0) {
		if (name_string == name) {
			return true;
		}
	}
	return false;
}

int identity_equal(const std::filesystem::path &left, const std::filesystem::path &right,
		   bool &same)
{
	const struct fs_mount_t *left_mount;
	const struct fs_mount_t *right_mount;
	std::string left_relative;
	std::string right_relative;
	int ret = find_mount(left, &left_mount, left_relative);

	if (ret == 0) {
		ret = find_mount(right, &right_mount, right_relative);
	}
	if (ret != 0) {
		return ret;
	}
	/*
	 * LittleFS has case-sensitive names and neither hard links nor symlinks.
	 * Resolved volume-relative names therefore identify its files. Other
	 * drivers need a real identity facility before using this comparison.
	 */
	if (left_mount->type != FS_LITTLEFS || right_mount->type != FS_LITTLEFS) {
		return ENOTSUP;
	}
	same = left_mount->fs_data == right_mount->fs_data && left_relative == right_relative;
	return 0;
}

} /* namespace zephyr_libcxx */

_LIBCPP_BEGIN_NAMESPACE_FILESYSTEM

using detail::ErrorHandler;

path __absolute(const path &input, error_code *ec)
{
	ErrorHandler<path> error("absolute", ec, &input);

	if (input.empty()) {
		error_code local;
		path current = __current_path(&local);

		return local ? error.report(local) : current / input;
	}
	path result;
	int ret = absolute_path(input, result);

	return ret == 0 ? result : error.report(native_error(ret));
}

path __canonical(const path &input, error_code *ec)
{
	ErrorHandler<path> error("canonical", ec, &input);
	path result;
	int ret = zephyr_libcxx::resolve(input, result);

	return ret == 0 ? result : error.report(native_error(ret));
}

path __weakly_canonical(const path &input, error_code *ec)
{
	ErrorHandler<path> error("weakly_canonical", ec, &input);
	path result;

	if (input.empty()) {
		return __current_path(ec);
	}
	int ret = resolve_path(input, result, false, true);

	return ret == 0 ? result : error.report(native_error(ret));
}

path __current_path(error_code *ec)
{
	ErrorHandler<path> error("current_path", ec);
	path result;

	{
		std::lock_guard<std::mutex> lock(cwd_lock);

		result = current_directory;
	}
	struct fs_dirent entry;
	int ret = fs_stat(result.c_str(), &entry);
	if (ret != 0) {
		return error.report(native_error(-ret));
	}
	if (entry.type != FS_DIR_ENTRY_DIR) {
		return error.report(errc::not_a_directory);
	}
	return result;
}

void __current_path(const path &input, error_code *ec)
{
	ErrorHandler<void> error("current_path", ec, &input);
	path absolute;
	struct fs_dirent entry;
	int ret = stat_path(input, absolute, entry);

	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (entry.type != FS_DIR_ENTRY_DIR) {
		return error.report(errc::not_a_directory);
	}
	std::lock_guard<std::mutex> lock(cwd_lock);

	memcpy(current_directory, absolute.c_str(), absolute.native().size() + 1);
}

file_status __status(const path &input, error_code *ec)
{
	ErrorHandler<void> error("status", ec, &input);
	path absolute;
	struct fs_dirent entry;
	int ret = stat_path(input, absolute, entry);

	if (ret == ENOENT || ret == ENOTDIR) {
		if (ec != nullptr) {
			*ec = native_error(ret);
		}
		return file_status(file_type::not_found);
	}
	if (ret != 0) {
		error.report(native_error(ret));
		return file_status(file_type::none);
	}
	file_type type = file_type::unknown;

	if (entry.type == FS_DIR_ENTRY_DIR) {
		type = file_type::directory;
	} else if (entry.type == FS_DIR_ENTRY_FILE) {
		type = file_type::regular;
	}

	/* Zephyr fs_stat has no permission bits; unknown is not equivalent to none. */
	return file_status(type, perms::unknown);
}

file_status __symlink_status(const path &input, error_code *ec)
{
	return __status(input, ec);
}

bool __equivalent(const path &left, const path &right, error_code *ec)
{
	ErrorHandler<bool> error("equivalent", ec, &left, &right);
	path first;
	path second;
	bool same = false;
	int ret = zephyr_libcxx::resolve(left, first);

	if (ret == 0) {
		ret = zephyr_libcxx::resolve(right, second);
	}
	if (ret == 0) {
		if (first == second) {
			return true;
		}
		ret = zephyr_libcxx::identity_equal(first, second, same);
	}
	return ret == 0 ? same : error.report(native_error(ret));
}

uintmax_t __file_size(const path &input, error_code *ec)
{
	ErrorHandler<uintmax_t> error("file_size", ec, &input);
	path absolute;
	struct fs_dirent entry;
	int ret = stat_path(input, absolute, entry);

	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (entry.type != FS_DIR_ENTRY_FILE) {
		return error.report(errc::is_a_directory);
	}
	return entry.size;
}

bool __fs_is_empty(const path &input, error_code *ec)
{
	ErrorHandler<bool> error("is_empty", ec, &input);
	error_code local;
	file_status status = __status(input, &local);

	if (local) {
		return error.report(local);
	}
	if (is_regular_file(status)) {
		auto size = __file_size(input, &local);

		return local ? error.report(local) : size == 0;
	}
	if (is_directory(status)) {
		directory_iterator iterator(input, local);

		return local ? error.report(local) : iterator == directory_iterator();
	}
	return error.report(errc::not_supported);
}

bool __create_directory(const path &input, error_code *ec)
{
	ErrorHandler<bool> error("create_directory", ec, &input);
	path absolute;
	int ret = zephyr_libcxx::resolve(input, absolute, true);

	if (ret != 0) {
		return error.report(native_error(ret));
	}
	struct fs_dirent entry;

	ret = fs_stat(absolute.c_str(), &entry);
	if (ret == 0) {
		return entry.type == FS_DIR_ENTRY_DIR ? false : error.report(errc::file_exists);
	}
	if (ret != -ENOENT) {
		return error.report(native_error(-ret));
	}
	ret = fs_mkdir(absolute.c_str());
	if (ret == -EEXIST) {
		ret = fs_stat(absolute.c_str(), &entry);
		if (ret == 0 && entry.type == FS_DIR_ENTRY_DIR) {
			return false;
		}
		return error.report(errc::file_exists);
	}
	return ret == 0 ? true : error.report(native_error(-ret));
}

bool __create_directory(const path &input, const path &attributes, error_code *ec)
{
	ErrorHandler<bool> error("create_directory", ec, &input, &attributes);
	error_code local;
	file_status source = __status(attributes, &local);

	if (local) {
		return error.report(local);
	}
	if (!is_directory(source)) {
		return error.report(errc::not_a_directory);
	}
	/* The native filesystem has no directory permission attributes to copy. */
	const bool created = __create_directory(input, &local);

	return local ? error.report(local) : created;
}

bool __create_directories(const path &input, error_code *ec)
{
	ErrorHandler<bool> error("create_directories", ec, &input);
	path absolute;
	int ret = absolute_path(input, absolute);

	if (ret != 0) {
		return error.report(native_error(ret));
	}
	path current("/");
	bool created = false;
	error_code local;
	auto part = absolute.begin();

	for (++part; part != absolute.end(); ++part) {
		if (part->empty()) {
			continue;
		}
		current /= *part;
		created = __create_directory(current, &local);
		if (local) {
			auto next = part;

			if (local == errc::file_exists && ++next != absolute.end()) {
				return error.report(errc::not_a_directory);
			}
			return error.report(local);
		}
	}
	return created;
}

bool __remove(const path &input, error_code *ec)
{
	ErrorHandler<bool> error("remove", ec, &input);

	if (final_dot_component(input)) {
		return error.report(errc::invalid_argument);
	}
	path absolute;
	int ret = zephyr_libcxx::resolve(input, absolute);

	if (ret == ENOENT) {
		return false;
	}
	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (zephyr_libcxx::mount_root(absolute)) {
		return error.report(errc::device_or_resource_busy);
	}
	ret = fs_unlink(absolute.c_str());
	if (ret == -ENOENT) {
		return false;
	}
	return ret == 0 ? true : error.report(native_error(-ret));
}

void __rename(const path &from, const path &to, error_code *ec)
{
	ErrorHandler<void> error("rename", ec, &from, &to);

	if (final_dot_component(from) || final_dot_component(to)) {
		return error.report(errc::invalid_argument);
	}
	path source;
	path target;
	int ret = zephyr_libcxx::resolve(from, source);

	if (ret == 0) {
		ret = zephyr_libcxx::resolve(to, target, true);
	}
	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (zephyr_libcxx::mount_root(source) || zephyr_libcxx::mount_root(target)) {
		return error.report(errc::device_or_resource_busy);
	}
	const struct fs_mount_t *source_mount;
	const struct fs_mount_t *target_mount;
	std::string ignored;
	ret = find_mount(source, &source_mount, ignored);
	if (ret == 0) {
		ret = find_mount(target, &target_mount, ignored);
	}
	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (source_mount != target_mount) {
		return error.report(errc::cross_device_link);
	}
	if (!to.empty() && to.native().back() == '/') {
		struct fs_dirent source_entry;
		struct fs_dirent target_entry;

		ret = fs_stat(source.c_str(), &source_entry);
		if (ret != 0) {
			return error.report(native_error(-ret));
		}
		if (source_entry.type == FS_DIR_ENTRY_FILE) {
			ret = fs_stat(target.c_str(), &target_entry);
			if (ret != 0) {
				return error.report(native_error(-ret));
			}
		}
	}
	ret = fs_rename(source.c_str(), target.c_str());
	if (ret != 0) {
		error.report(native_error(-ret));
	}
}

void __resize_file(const path &input, uintmax_t size, error_code *ec)
{
	ErrorHandler<void> error("resize_file", ec, &input);
	path absolute;
	struct fs_dirent entry;
	int ret = stat_path(input, absolute, entry);

	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (entry.type != FS_DIR_ENTRY_FILE) {
		return error.report(errc::is_a_directory);
	}
	if (size > static_cast<uintmax_t>(std::numeric_limits<off_t>::max())) {
		return error.report(errc::file_too_large);
	}
	struct fs_file_t file;

	fs_file_t_init(&file);
	ret = fs_open(&file, absolute.c_str(), FS_O_WRITE);
	if (ret != 0) {
		return error.report(native_error(-ret));
	}
	int truncate_ret = fs_truncate(&file, static_cast<off_t>(size));
	int close_ret = fs_close(&file);

	if (truncate_ret != 0 || close_ret != 0) {
		error.report(native_error(truncate_ret != 0 ? -truncate_ret : -close_ret));
	}
}

space_info __space(const path &input, error_code *ec)
{
	ErrorHandler<void> error("space", ec, &input);
	constexpr uintmax_t unknown = static_cast<uintmax_t>(-1);
	path absolute;
	int ret = zephyr_libcxx::resolve(input, absolute);
	struct fs_statvfs information{};

	if (ret == 0) {
		ret = absolute == "/" ? ENOTSUP : -fs_statvfs(absolute.c_str(), &information);
	}
	if (ret != 0) {
		error.report(native_error(ret));
		return {unknown, unknown, unknown};
	}
	uintmax_t block_size = information.f_frsize;

	if (block_size == 0 || information.f_blocks > unknown / block_size ||
	    information.f_bfree > unknown / block_size) {
		error.report(errc::value_too_large);
		return {unknown, unknown, unknown};
	}
	uintmax_t free = block_size * information.f_bfree;

	return {block_size * information.f_blocks, free, free};
}

path __temp_directory_path(error_code *ec)
{
	ErrorHandler<path> error("temp_directory_path", ec);
	const path configured(CONFIG_LIBCXX_FILESYSTEM_TEMP_DIR);
	path absolute;
	struct fs_dirent entry;
	int ret = stat_path(configured, absolute, entry);

	if (ret != 0) {
		return error.report(native_error(ret));
	}
	if (entry.type != FS_DIR_ENTRY_DIR) {
		return error.report(errc::not_a_directory);
	}
	return absolute;
}

uintmax_t __hard_link_count(const path &input, error_code *ec)
{
	ErrorHandler<uintmax_t> error("hard_link_count", ec, &input);

	return error.report(native_error(unsupported_metadata(input)));
}

file_time_type __last_write_time(const path &input, error_code *ec)
{
	ErrorHandler<file_time_type> error("last_write_time", ec, &input);

	return error.report(native_error(unsupported_metadata(input)));
}

void __last_write_time(const path &input, file_time_type, error_code *ec)
{
	ErrorHandler<void> error("last_write_time", ec, &input);

	error.report(native_error(unsupported_metadata(input)));
}

void __permissions(const path &input, perms, perm_options, error_code *ec)
{
	ErrorHandler<void> error("permissions", ec, &input);

	error.report(native_error(unsupported_metadata(input)));
}

path __read_symlink(const path &input, error_code *ec)
{
	ErrorHandler<path> error("read_symlink", ec, &input);

	return error.report(native_error(unsupported_metadata(input)));
}

void __create_symlink(const path &target, const path &link, error_code *ec)
{
	ErrorHandler<void> error("create_symlink", ec, &target, &link);

	error.report(errc::not_supported);
}

void __create_directory_symlink(const path &target, const path &link, error_code *ec)
{
	__create_symlink(target, link, ec);
}

void __create_hard_link(const path &target, const path &link, error_code *ec)
{
	ErrorHandler<void> error("create_hard_link", ec, &target, &link);

	error.report(errc::not_supported);
}

void __copy_symlink(const path &source, const path &target, error_code *ec)
{
	ErrorHandler<void> error("copy_symlink", ec, &source, &target);

	error.report(errc::not_supported);
}

_LIBCPP_END_NAMESPACE_FILESYSTEM
