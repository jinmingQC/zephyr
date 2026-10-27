/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/ztest.h>

namespace fs = std::filesystem;

BUILD_ASSERT(__cplusplus == 201703L);

namespace
{

constexpr const char *mount_path = "/lfs";
constexpr const char *test_path = "/lfs/test";
const char disk_name[] = "LIBCXX";
struct fs_littlefs storage;
struct fs_mount_t mount;
uint8_t read_cache[512];
uint8_t prog_cache[512];
uint32_t lookahead[8];

fs::path child(const char *name)
{
	return fs::path(test_path) / name;
}

std::error_code stale_error()
{
	return std::make_error_code(std::errc::io_error);
}

std::vector<uint8_t> pattern(size_t size = 8193, uint8_t seed = 0x31)
{
	std::vector<uint8_t> result(size);

	for (size_t i = 0; i < size; ++i) {
		result[i] = static_cast<uint8_t>((i * 37U + seed) & 0xffU);
	}
	return result;
}

void write_data(const fs::path &path, const std::vector<uint8_t> &data)
{
	struct fs_file_t file;

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, path.c_str(), FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
	size_t offset = 0;

	while (offset < data.size()) {
		const ssize_t written = fs_write(&file, data.data() + offset, data.size() - offset);

		zassert_true(written > 0, "Native write failed: %d", static_cast<int>(written));
		offset += static_cast<size_t>(written);
	}
	zassert_ok(fs_close(&file));
}

std::vector<uint8_t> read_data(const fs::path &path)
{
	struct fs_dirent info;
	struct fs_file_t file;

	zassert_ok(fs_stat(path.c_str(), &info));
	std::vector<uint8_t> result(info.size);
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, path.c_str(), FS_O_READ));
	size_t offset = 0;

	while (offset < result.size()) {
		const ssize_t count =
			fs_read(&file, result.data() + offset, result.size() - offset);

		zassert_true(count > 0, "Native read failed: %d", static_cast<int>(count));
		offset += static_cast<size_t>(count);
	}
	uint8_t extra;

	zassert_equal(fs_read(&file, &extra, sizeof(extra)), 0, "Unexpected bytes after EOF");
	zassert_ok(fs_close(&file));
	return result;
}

void require_directory(const fs::path &path)
{
	std::error_code ec;

	fs::create_directories(path, ec);
	zassert_false(static_cast<bool>(ec), "Cannot create directory: %d", ec.value());
}

void *setup()
{
	storage.cfg.read_buffer = read_cache;
	storage.cfg.prog_buffer = prog_cache;
	storage.cfg.lookahead_buffer = lookahead;
	storage.cfg.read_size = sizeof(read_cache);
	storage.cfg.prog_size = sizeof(prog_cache);
	storage.cfg.cache_size = sizeof(read_cache);
	storage.cfg.lookahead_size = sizeof(lookahead);
	mount.type = FS_LITTLEFS;
	mount.fs_data = &storage;
	mount.storage_dev = const_cast<char *>(disk_name);
	mount.mnt_point = mount_path;
	mount.flags = FS_MOUNT_FLAG_USE_DISK_ACCESS | FS_MOUNT_FLAG_NO_FORMAT;

	zassert_ok(fs_mkfs(FS_LITTLEFS, reinterpret_cast<uintptr_t>(disk_name), nullptr,
			   FS_MOUNT_FLAG_USE_DISK_ACCESS));
	zassert_ok(fs_mount(&mount));
	return nullptr;
}

void before(void *)
{
	std::error_code ec;

	fs::current_path(mount_path, ec);
	zassert_false(static_cast<bool>(ec));
	fs::remove_all(test_path, ec);
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::create_directory(test_path, ec));
	zassert_false(static_cast<bool>(ec));
}

void teardown(void *)
{
	zassert_ok(fs_unmount(&mount));
}

} /* namespace */

ZTEST(libcxx_filesystem, test_lexical_paths)
{
	const fs::path path("/lfs/dir/file.tar.gz");

	zassert_true(path.is_absolute());
	zassert_equal(path.root_path(), fs::path("/"));
	zassert_equal(path.parent_path(), fs::path("/lfs/dir"));
	zassert_equal(path.filename(), fs::path("file.tar.gz"));
	zassert_equal(path.stem(), fs::path("file.tar"));
	zassert_equal(path.extension(), fs::path(".gz"));
	zassert_equal(fs::path("a/./b/../c").lexically_normal(), fs::path("a/c"));
	zassert_equal(fs::path("a/b").lexically_relative("a/c"), fs::path("../b"));
	zassert_equal((fs::path("/lfs") / "dir" / "file"), fs::path("/lfs/dir/file"));
}

ZTEST(libcxx_filesystem, test_create_directories_and_status)
{
	std::error_code ec = stale_error();

	zassert_true(fs::create_directory(child("single"), ec));
	zassert_false(static_cast<bool>(ec));
	ec = stale_error();
	zassert_false(fs::create_directory(child("single"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::create_directories(child("nested/inner"), ec));
	zassert_false(static_cast<bool>(ec));
	const auto status = fs::status(child("nested/inner"), ec);

	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::is_directory(status));
	zassert_equal(status.permissions(), fs::perms::unknown);
	zassert_false(fs::is_regular_file(status));
}

ZTEST(libcxx_filesystem, test_file_size_and_empty)
{
	write_data(child("data"), pattern(67));
	write_data(child("empty"), {});
	std::error_code ec = stale_error();

	zassert_true(fs::is_regular_file(child("data"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_equal(fs::file_size(child("data"), ec), 67);
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::is_empty(child("data"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::is_empty(child("empty"), ec));
	zassert_false(static_cast<bool>(ec));
	require_directory(child("empty-dir"));
	zassert_true(fs::is_empty(child("empty-dir"), ec));
	zassert_false(static_cast<bool>(ec));
	write_data(child("empty-dir/child"), {});
	zassert_false(fs::is_empty(child("empty-dir"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_missing_paths)
{
	std::error_code ec = stale_error();

	zassert_false(fs::exists(child("missing"), ec));
	zassert_false(static_cast<bool>(ec), "exists must clear errors for known missing paths");
	zassert_equal(fs::status(child("missing"), ec).type(), fs::file_type::not_found);
	zassert_equal(fs::file_size(child("missing"), ec), std::numeric_limits<uintmax_t>::max());
	zassert_true(ec == std::errc::no_such_file_or_directory);
}

ZTEST(libcxx_filesystem, test_directory_creation_errors)
{
	write_data(child("file"), {});
	std::error_code ec;

	zassert_false(fs::create_directory(child("file"), ec));
	zassert_true(ec == std::errc::file_exists);
	zassert_false(fs::create_directory(child("missing/child"), ec));
	zassert_true(ec == std::errc::no_such_file_or_directory);
	zassert_false(fs::create_directories(child("file/child"), ec));
	zassert_true(ec == std::errc::not_a_directory);
}

ZTEST(libcxx_filesystem, test_copy_file_contents)
{
	const auto data = pattern();

	write_data(child("source"), data);
	std::error_code ec = stale_error();

	zassert_true(fs::copy_file(child("source"), child("copy"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("copy")) == data, "Copied bytes differ from source");
	zassert_true(read_data(child("source")) == data, "Copy modified source");
	zassert_equal(fs::file_size(child("copy"), ec), data.size());
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_equivalent_uses_identity)
{
	write_data(child("first"), pattern(31));
	write_data(child("second"), pattern(31));
	require_directory(child("directory"));
	std::error_code ec = stale_error();

	zassert_false(fs::equivalent(child("first"), child("second"), ec),
		      "Different files must not compare equal because stat inode is zero");
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::equivalent(child("first"), child("directory/../first"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::equivalent(child("first"), child("missing"), ec));
	zassert_true(ec == std::errc::no_such_file_or_directory);
}

ZTEST(libcxx_filesystem, test_copy_existing_fails_without_damage)
{
	const auto source = pattern(257, 1);
	const auto original = pattern(513, 2);

	write_data(child("source"), source);
	write_data(child("destination"), original);
	std::error_code ec;

	zassert_false(fs::copy_file(child("source"), child("destination"), ec));
	zassert_true(ec == std::errc::file_exists);
	zassert_true(read_data(child("destination")) == original);
	zassert_true(read_data(child("source")) == source);
}

ZTEST(libcxx_filesystem, test_copy_skip_existing)
{
	const auto original = pattern(513, 2);

	write_data(child("source"), pattern(257, 1));
	write_data(child("destination"), original);
	std::error_code ec = stale_error();

	zassert_false(fs::copy_file(child("source"), child("destination"),
				    fs::copy_options::skip_existing, ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("destination")) == original);
}

ZTEST(libcxx_filesystem, test_copy_overwrite_existing)
{
	const auto data = pattern(1537, 1);

	write_data(child("source"), data);
	write_data(child("destination"), pattern(8193, 2));
	std::error_code ec = stale_error();

	zassert_true(fs::copy_file(child("source"), child("destination"),
				   fs::copy_options::overwrite_existing, ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("destination")) == data, "Overwrite must also truncate");
}

ZTEST(libcxx_filesystem, test_self_copy_does_not_truncate)
{
	const auto data = pattern();

	write_data(child("source"), data);
	require_directory(child("directory"));
	std::error_code ec;

	zassert_false(fs::copy_file(child("source"), child("directory/../source"),
				    fs::copy_options::overwrite_existing, ec));
	zassert_true(static_cast<bool>(ec));
	zassert_true(read_data(child("source")) == data);
}

ZTEST(libcxx_filesystem, test_copy_update_reports_unsupported)
{
	const auto source = pattern(257, 1);
	const auto destination = pattern(513, 2);

	write_data(child("source"), source);
	write_data(child("destination"), destination);
	std::error_code ec;

	zassert_false(fs::copy_file(child("source"), child("destination"),
				    fs::copy_options::update_existing, ec));
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	zassert_true(read_data(child("source")) == source);
	zassert_true(read_data(child("destination")) == destination);
}

ZTEST(libcxx_filesystem, test_copy_recursive_tree)
{
	require_directory(child("source/nested/empty"));
	const auto first = pattern(109);
	const auto second = pattern(4097);

	write_data(child("source/first.bin"), first);
	write_data(child("source/nested/second.bin"), second);
	std::error_code ec = stale_error();

	fs::copy(child("source"), child("destination"), fs::copy_options::recursive, ec);
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("destination/first.bin")) == first);
	zassert_true(read_data(child("destination/nested/second.bin")) == second);
	zassert_true(fs::is_directory(child("destination/nested/empty"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_rename_and_replace)
{
	const auto data = pattern(79);

	write_data(child("old"), data);
	write_data(child("new"), pattern(159, 2));
	std::error_code ec = stale_error();

	fs::rename(child("old"), child("new"), ec);
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::exists(child("old"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("new")) == data);
	fs::rename(child("absent"), child("new"), ec);
	zassert_true(ec == std::errc::no_such_file_or_directory);
	zassert_true(read_data(child("new")) == data);
}

ZTEST(libcxx_filesystem, test_remove_and_remove_all)
{
	require_directory(child("tree/sub"));
	require_directory(child("tree/empty"));
	write_data(child("tree/file"), pattern(7));
	write_data(child("tree/sub/file"), pattern(11));
	std::error_code ec = stale_error();

	zassert_equal(fs::remove_all(child("tree"), ec), 5);
	zassert_false(static_cast<bool>(ec));
	ec = stale_error();
	zassert_equal(fs::remove_all(child("tree"), ec), 0);
	zassert_false(static_cast<bool>(ec));
	write_data(child("file"), {});
	zassert_true(fs::remove(child("file"), ec));
	zassert_false(static_cast<bool>(ec));
	ec = stale_error();
	zassert_false(fs::remove(child("file"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_remove_nonempty_directory)
{
	require_directory(child("directory"));
	write_data(child("directory/file"), pattern(19));
	std::error_code ec;

	zassert_false(fs::remove(child("directory"), ec));
	zassert_true(ec == std::errc::directory_not_empty);
	zassert_equal(read_data(child("directory/file")).size(), 19);
}

ZTEST(libcxx_filesystem, test_directory_iterator)
{
	require_directory(child("directory"));
	write_data(child("first"), pattern(9));
	write_data(child("second"), pattern(17));
	std::error_code ec = stale_error();
	std::set<std::string> names;
	uint32_t count = 0;

	fs::directory_iterator entry(test_path, ec);
	zassert_false(static_cast<bool>(ec));
	for (; entry != fs::directory_iterator(); entry.increment(ec)) {
		zassert_false(static_cast<bool>(ec));
		zassert_true(++count <= 3, "Directory iteration did not terminate");
		names.insert(entry->path().filename().string());
	}
	zassert_false(static_cast<bool>(ec));
	zassert_true(names == std::set<std::string>({"directory", "first", "second"}));
}

ZTEST(libcxx_filesystem, test_recursive_directory_iterator)
{
	require_directory(child("directory/inner"));
	write_data(child("first"), {});
	write_data(child("directory/second"), {});
	write_data(child("directory/inner/third"), {});
	std::error_code ec;
	std::set<std::string> paths;
	int max_depth = 0;
	uint32_t count = 0;

	fs::recursive_directory_iterator entry(test_path, ec);
	zassert_false(static_cast<bool>(ec));
	for (; entry != fs::recursive_directory_iterator(); entry.increment(ec)) {
		zassert_false(static_cast<bool>(ec));
		zassert_true(++count <= 5, "Recursive iteration did not terminate");
		paths.insert(entry->path().lexically_relative(test_path).string());
		if (entry.depth() > max_depth) {
			max_depth = entry.depth();
		}
	}
	zassert_false(static_cast<bool>(ec));
	zassert_equal(max_depth, 2);
	zassert_true(paths ==
		     std::set<std::string>({"directory", "directory/inner", "directory/inner/third",
					    "directory/second", "first"}));
}

ZTEST(libcxx_filesystem, test_disable_recursion)
{
	require_directory(child("directory/inner"));
	write_data(child("first"), {});
	write_data(child("directory/inner/hidden"), {});
	std::error_code ec;
	uint32_t count = 0;

	fs::recursive_directory_iterator entry(test_path, ec);
	zassert_false(static_cast<bool>(ec));
	for (; entry != fs::recursive_directory_iterator(); entry.increment(ec)) {
		zassert_false(static_cast<bool>(ec));
		if (entry->path().filename() == "directory") {
			entry.disable_recursion_pending();
		}
		zassert_equal(entry.depth(), 0);
		zassert_true(++count < 4);
	}
	zassert_false(static_cast<bool>(ec));
	zassert_equal(count, 2);
}

ZTEST(libcxx_filesystem, test_iterator_errors)
{
	write_data(child("file"), {});
	std::error_code ec;

	fs::directory_iterator missing(child("missing"), ec);
	zassert_true(missing == fs::directory_iterator());
	zassert_true(ec == std::errc::no_such_file_or_directory);
	fs::directory_iterator regular(child("file"), ec);
	zassert_true(regular == fs::directory_iterator());
	zassert_true(ec == std::errc::not_a_directory);
}

ZTEST(libcxx_filesystem, test_directory_entry_refresh)
{
	write_data(child("file"), pattern(3));
	std::error_code ec = stale_error();
	fs::directory_entry entry(child("file"), ec);

	zassert_false(static_cast<bool>(ec));
	zassert_equal(entry.file_size(ec), 3);
	zassert_false(static_cast<bool>(ec));
	write_data(child("file"), pattern(73));
	entry.refresh(ec);
	zassert_false(static_cast<bool>(ec));
	zassert_equal(entry.file_size(ec), 73);
	zassert_false(static_cast<bool>(ec));
	zassert_true(entry.is_regular_file(ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_space_uses_byte_units)
{
	std::error_code ec = stale_error();
	const auto initial = fs::space(test_path, ec);

	zassert_false(static_cast<bool>(ec));
	zassert_equal(initial.capacity, 512U * 256U);
	zassert_true(initial.free <= initial.capacity);
	zassert_true(initial.available <= initial.free);
	write_data(child("allocated"), pattern(8193));
	const auto after = fs::space(test_path, ec);

	zassert_false(static_cast<bool>(ec));
	zassert_equal(after.capacity, initial.capacity);
	zassert_true(after.free < initial.free, "Stored data did not consume filesystem space");
}

ZTEST(libcxx_filesystem, test_resize_shrink_and_zero_extend)
{
	const auto data = pattern(257);

	write_data(child("file"), data);
	std::error_code ec = stale_error();

	fs::resize_file(child("file"), 17, ec);
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("file")) ==
		     std::vector<uint8_t>(data.begin(), data.begin() + 17));
	fs::resize_file(child("file"), 4097, ec);
	zassert_false(static_cast<bool>(ec));
	const auto extended = read_data(child("file"));

	zassert_equal(extended.size(), 4097);
	for (size_t i = 0; i < extended.size(); ++i) {
		zassert_equal(extended[i], i < 17 ? data[i] : 0, "Unexpected byte at %u",
			      static_cast<unsigned int>(i));
	}
	zassert_equal(fs::file_size(child("file"), ec), extended.size());
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_resize_missing_does_not_create)
{
	std::error_code ec;

	fs::resize_file(child("missing"), 37, ec);
	zassert_true(ec == std::errc::no_such_file_or_directory);
	zassert_false(fs::exists(child("missing"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_current_path_and_relative_lookup)
{
	write_data(child("file"), pattern(9));
	std::error_code ec = stale_error();

	fs::current_path(test_path, ec);
	zassert_false(static_cast<bool>(ec));
	zassert_equal(fs::current_path(ec), fs::path(test_path));
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::exists("file", ec));
	zassert_false(static_cast<bool>(ec));
	zassert_equal(fs::absolute("file", ec), child("file"));
	zassert_false(static_cast<bool>(ec));
	ec = stale_error();
	zassert_equal(fs::absolute("", ec), fs::path(test_path) / "");
	zassert_false(static_cast<bool>(ec));
	fs::current_path(mount_path, ec);
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_current_path_errors_preserve_state)
{
	write_data(child("file"), {});
	std::error_code ec;
	const auto previous = fs::current_path(ec);

	zassert_false(static_cast<bool>(ec));
	fs::current_path(child("missing"), ec);
	zassert_true(ec == std::errc::no_such_file_or_directory);
	zassert_equal(fs::current_path(ec), previous);
	zassert_false(static_cast<bool>(ec));
	fs::current_path(child("file"), ec);
	zassert_true(ec == std::errc::not_a_directory);
	zassert_equal(fs::current_path(ec), previous);
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_canonical_checks_components)
{
	require_directory(child("directory"));
	write_data(child("file"), {});
	std::error_code ec = stale_error();

	zassert_equal(fs::canonical(child("directory/../file"), ec), child("file"));
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::canonical(child("file/.."), ec).empty());
	zassert_true(ec == std::errc::not_a_directory);
	zassert_true(fs::canonical(child("file/child"), ec).empty());
	zassert_true(ec == std::errc::not_a_directory);
	zassert_true(fs::canonical(child("missing/.."), ec).empty());
	zassert_true(ec == std::errc::no_such_file_or_directory);
}

ZTEST(libcxx_filesystem, test_weakly_canonical_missing_suffix)
{
	require_directory(child("directory"));
	write_data(child("file"), {});
	std::error_code ec = stale_error();

	zassert_equal(fs::weakly_canonical(child("directory/missing/../leaf"), ec),
		      child("directory/leaf"));
	zassert_false(static_cast<bool>(ec));
	const auto cwd = fs::current_path(ec);

	zassert_false(static_cast<bool>(ec));
	ec = stale_error();
	zassert_equal(fs::weakly_canonical("", ec), cwd);
	zassert_false(static_cast<bool>(ec));
	ec = stale_error();
	/* A missing suffix ending in ".." retains its lexical trailing separator. */
	zassert_equal(fs::weakly_canonical(child("file/child/.."), ec),
		      child("file/child/..").lexically_normal());
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_temp_directory_must_exist)
{
	const fs::path temporary(CONFIG_LIBCXX_FILESYSTEM_TEMP_DIR);
	std::error_code ec;

	fs::remove_all(temporary, ec);
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::temp_directory_path(ec).empty());
	zassert_true(ec == std::errc::no_such_file_or_directory);
	require_directory(temporary);
	ec = stale_error();
	zassert_equal(fs::temp_directory_path(ec), temporary);
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::remove(temporary, ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_unsupported_metadata_and_links)
{
	write_data(child("file"), pattern(7));
	std::error_code ec;

	zassert_equal(fs::last_write_time(child("file"), ec), fs::file_time_type::min());
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	fs::last_write_time(child("file"), fs::file_time_type{}, ec);
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	fs::permissions(child("file"), fs::perms::owner_read, ec);
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	zassert_equal(fs::hard_link_count(child("file"), ec),
		      std::numeric_limits<uintmax_t>::max());
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	fs::create_hard_link(child("file"), child("hard-link"), ec);
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	fs::create_symlink(child("file"), child("symlink"), ec);
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	fs::create_directory_symlink(test_path, child("directory-link"), ec);
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	zassert_true(fs::read_symlink(child("file"), ec).empty());
	zassert_true(ec == std::errc::not_supported, "Expected ENOTSUP, got %d", ec.value());
	zassert_true(fs::is_regular_file(fs::symlink_status(child("file"), ec)));
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::exists(child("hard-link"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::exists(child("symlink"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_long_component_reports_error)
{
	std::error_code ec;
	const auto path = fs::path(test_path) / std::string(300, 'x');

	zassert_false(fs::create_directory(path, ec));
	zassert_true(ec == std::errc::filename_too_long);
}

ZTEST(libcxx_filesystem, test_concurrent_directory_streams)
{
	for (uint32_t i = 0; i < 5; ++i) {
		write_data(child(("file" + std::to_string(i)).c_str()), pattern(i + 1));
	}
	std::atomic<uint32_t> failures{0};
	std::array<std::thread, 4> workers;

	for (auto &worker : workers) {
		worker = std::thread([&] {
			for (uint32_t pass = 0; pass < 20; ++pass) {
				std::error_code ec;
				uint32_t mask = 0;
				uint32_t count = 0;
				fs::directory_iterator entry(test_path, ec);

				for (; !ec && entry != fs::directory_iterator();
				     entry.increment(ec)) {
					const auto name = entry->path().filename().string();

					if (name.size() == 5 && name.compare(0, 4, "file") == 0 &&
					    name[4] >= '0' && name[4] <= '4') {
						mask |= 1U << (name[4] - '0');
					} else {
						failures.fetch_add(1);
					}
					if (++count > 5) {
						break;
					}
					std::this_thread::yield();
				}
				if (ec || count != 5 || mask != 0x1fU) {
					failures.fetch_add(1);
				}
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	zassert_equal(failures.load(), 0, "Independent directory streams interfered");
}

ZTEST(libcxx_filesystem, test_data_survives_remount)
{
	const auto data = pattern();

	write_data(child("persistent"), data);
	zassert_ok(fs_unmount(&mount));
	zassert_ok(fs_mount(&mount));
	std::error_code ec = stale_error();

	zassert_true(fs::exists(child("persistent"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("persistent")) == data);
}

ZTEST(libcxx_filesystem, test_copy_missing_destination_with_trailing_slash)
{
	const auto data = pattern();

	write_data(child("source"), data);
	std::error_code ec;

	zassert_false(fs::copy_file(child("source"), child("missing/"), ec));
	zassert_true(static_cast<bool>(ec), "A trailing slash requires a directory");
	zassert_true(read_data(child("source")) == data);
	zassert_false(fs::exists(child("missing"), ec), "Failed copy created a regular file");
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_permissions_missing_path)
{
	std::error_code ec;

	fs::permissions(child("missing"), fs::perms::owner_read, ec);
	zassert_true(ec == std::errc::no_such_file_or_directory);
	zassert_false(fs::exists(child("missing"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_last_write_time_missing_path)
{
	std::error_code ec;

	zassert_equal(fs::last_write_time(child("missing"), ec), fs::file_time_type::min());
	zassert_true(ec == std::errc::no_such_file_or_directory);
	fs::last_write_time(child("missing"), fs::file_time_type{}, ec);
	zassert_true(ec == std::errc::no_such_file_or_directory);
	zassert_false(fs::exists(child("missing"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_filesystem, test_remove_all_protects_mount_roots)
{
	const auto data = pattern(513);

	write_data(child("preserved"), data);
	std::error_code ec;

	zassert_equal(fs::remove_all("/", ec), std::numeric_limits<uintmax_t>::max());
	zassert_true(ec == std::errc::device_or_resource_busy);
	zassert_true(read_data(child("preserved")) == data, "Removing root changed mounted files");
	zassert_equal(fs::remove_all(mount_path, ec), std::numeric_limits<uintmax_t>::max());
	zassert_true(ec == std::errc::device_or_resource_busy);
	zassert_true(read_data(child("preserved")) == data,
		     "Removing mount root changed its files");
}

ZTEST(libcxx_filesystem, test_remove_rejects_final_dot_components)
{
	require_directory(child("empty"));
	require_directory(child("parent/nested"));
	const auto data = pattern(513);

	write_data(child("parent/preserved"), data);
	for (const char *name : {"empty/.", "empty/./", "parent/nested/..", "parent/nested/../"}) {
		std::error_code ec;

		zassert_false(fs::remove(child(name), ec));
		zassert_true(static_cast<bool>(ec), "remove accepted a final dot component");
		zassert_true(fs::is_directory(child("empty"), ec));
		zassert_false(static_cast<bool>(ec));
		zassert_true(fs::is_directory(child("parent/nested"), ec));
		zassert_false(static_cast<bool>(ec));
		zassert_true(read_data(child("parent/preserved")) == data);
	}
}

ZTEST(libcxx_filesystem, test_remove_all_rejects_final_dot_components)
{
	require_directory(child("parent/nested"));
	const auto data = pattern(513);

	write_data(child("parent/preserved"), data);
	for (const char *name :
	     {"parent/.", "parent/./", "parent/nested/..", "parent/nested/../"}) {
		std::error_code ec;

		zassert_equal(fs::remove_all(child(name), ec),
			      std::numeric_limits<uintmax_t>::max());
		zassert_true(static_cast<bool>(ec), "remove_all accepted a final dot component");
		zassert_true(read_data(child("parent/preserved")) == data);
		zassert_true(fs::is_directory(child("parent/nested"), ec));
		zassert_false(static_cast<bool>(ec));
	}
}

ZTEST(libcxx_filesystem, test_rename_rejects_final_dot_source)
{
	require_directory(child("source/nested"));
	const auto data = pattern(513);

	write_data(child("source/preserved"), data);
	for (const char *name :
	     {"source/.", "source/./", "source/nested/..", "source/nested/../"}) {
		std::error_code ec;

		fs::rename(child(name), child("renamed"), ec);
		zassert_true(static_cast<bool>(ec), "rename accepted a final dot source");
		zassert_true(read_data(child("source/preserved")) == data);
		zassert_true(fs::is_directory(child("source/nested"), ec));
		zassert_false(static_cast<bool>(ec));
		zassert_false(fs::exists(child("renamed"), ec));
		zassert_false(static_cast<bool>(ec));
	}
}

ZTEST(libcxx_filesystem, test_rename_rejects_final_dot_destination)
{
	require_directory(child("source"));
	require_directory(child("empty"));
	require_directory(child("target/nested"));
	const auto data = pattern(513);

	write_data(child("source/preserved"), data);
	for (const char *name : {"empty/.", "empty/./", "target/nested/..", "target/nested/../"}) {
		std::error_code ec;

		fs::rename(child("source"), child(name), ec);
		zassert_true(static_cast<bool>(ec), "rename accepted a final dot destination");
		zassert_true(read_data(child("source/preserved")) == data);
		zassert_true(fs::is_empty(child("empty"), ec));
		zassert_false(static_cast<bool>(ec));
		zassert_true(fs::is_directory(child("target/nested"), ec));
		zassert_false(static_cast<bool>(ec));
	}
}

ZTEST(libcxx_filesystem, test_recursive_copy_rejects_descendant)
{
	require_directory(child("source/nested"));
	const auto data = pattern();

	write_data(child("source/preserved"), data);
	write_data(child("source/nested/leaf"), pattern(37));
	for (const char *name : {"source/copy", "source/nested/copy"}) {
		std::error_code ec;

		fs::copy(child("source"), child(name), fs::copy_options::recursive, ec);
		zassert_true(static_cast<bool>(ec), "Recursive copy accepted its descendant");
		zassert_false(fs::exists(child(name), ec),
			      "Rejected recursive copy created its destination");
		zassert_false(static_cast<bool>(ec));
		zassert_true(read_data(child("source/preserved")) == data);
		zassert_true(read_data(child("source/nested/leaf")) == pattern(37));
	}
}

ZTEST(libcxx_filesystem, test_copy_full_disk_preserves_source_and_releases_handles)
{
	const auto data = pattern();

	write_data(child("source"), data);
	write_data(child("destination"), {});
	struct fs_file_t filler;

	fs_file_t_init(&filler);
	zassert_ok(fs_open(&filler, child("filler").c_str(), FS_O_CREATE | FS_O_WRITE));
	std::array<uint8_t, 512> block{};
	bool full = false;

	/* Commit each block so closing after ENOSPC cannot discard all of the filler. */
	for (size_t i = 0; i < 512; ++i) {
		const ssize_t written = fs_write(&filler, block.data(), block.size());

		if (written == -ENOSPC) {
			full = true;
			break;
		}
		zassert_equal(written, static_cast<ssize_t>(block.size()));
		const int synced = fs_sync(&filler);

		if (synced == -ENOSPC) {
			full = true;
			break;
		}
		zassert_ok(synced);
	}
	const int closed = fs_close(&filler);

	zassert_true(closed == 0 || closed == -ENOSPC);
	zassert_true(full, "Filler did not exhaust the 128 KiB RAM disk");
	for (size_t attempt = 0; attempt < CONFIG_FS_LITTLEFS_NUM_FILES + 2U; ++attempt) {
		std::error_code ec;

		zassert_false(fs::copy_file(child("source"), child("destination"),
					    fs::copy_options::overwrite_existing, ec));
		zassert_true(ec == std::errc::no_space_on_device,
			     "Expected ENOSPC, got %d (possibly a leaked handle)", ec.value());
		zassert_true(read_data(child("source")) == data,
			     "Failed copy changed its source or leaked open handles");
	}
	std::error_code ec;

	zassert_true(fs::remove(child("filler"), ec));
	zassert_false(static_cast<bool>(ec));
	/* A failed copy may leave a partial destination; the next overwrite must recover. */
	zassert_true(fs::copy_file(child("source"), child("destination"),
				   fs::copy_options::overwrite_existing, ec));
	zassert_false(static_cast<bool>(ec));
	zassert_true(read_data(child("source")) == data);
	zassert_true(read_data(child("destination")) == data);
}

ZTEST_SUITE(libcxx_filesystem, nullptr, setup, before, nullptr, teardown);
