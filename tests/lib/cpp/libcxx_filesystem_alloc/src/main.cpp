/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <new>
#include <system_error>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/ztest.h>

namespace fs = std::filesystem;

BUILD_ASSERT(IS_ENABLED(CONFIG_CPP_EXCEPTIONS));

namespace
{

std::atomic<int> failure_after{-1};
std::atomic<bool> failure_hit{false};

} /* namespace */

/* Replace the standard allocation point only in this fault-injection image. */
void *operator new(std::size_t size)
{
	int remaining = failure_after.load();

	while (remaining >= 0) {
		if (failure_after.compare_exchange_weak(remaining, remaining - 1)) {
			if (remaining == 0) {
				failure_hit.store(true);
				throw std::bad_alloc();
			}
			break;
		}
	}
	for (;;) {
		if (void *memory = std::malloc(size == 0 ? 1 : size)) {
			return memory;
		}
		const std::new_handler handler = std::get_new_handler();

		if (handler == nullptr) {
			throw std::bad_alloc();
		}
		handler();
	}
}

namespace
{

constexpr const char *mount_path = "/lfs";
constexpr const char *test_path = "/lfs/alloc";
const char disk_name[] = "LIBCXX";
struct fs_littlefs storage;
struct fs_mount_t mount;
uint8_t read_cache[512];
uint8_t prog_cache[512];
uint32_t lookahead[8];

class failure_scope
{
      public:
	explicit failure_scope(int position)
	{
		failure_hit.store(false);
		failure_after.store(position);
	}

	~failure_scope()
	{
		failure_after.store(-1);
	}

	failure_scope(const failure_scope &) = delete;
	failure_scope &operator=(const failure_scope &) = delete;
};

template <typename Operation> std::error_code fail_next(Operation operation)
{
	std::error_code ec;

	{
		failure_scope failure(0);

		operation(ec);
	}
	zassert_true(failure_hit.load(), "The operation did not allocate");
	zassert_true(ec == std::errc::not_enough_memory, "Expected ENOMEM, got %d", ec.value());
	return ec;
}

fs::path child(const char *name)
{
	return fs::path(test_path) / name;
}

void create_file(const fs::path &name)
{
	struct fs_file_t file;

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, name.c_str(), FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
	zassert_equal(fs_write(&file, "preserve", 8), 8);
	zassert_ok(fs_close(&file));
}

void check_file(const fs::path &name)
{
	struct fs_file_t file;
	char bytes[8]{};

	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, name.c_str(), FS_O_READ));
	zassert_equal(fs_read(&file, bytes, sizeof(bytes)), sizeof(bytes));
	zassert_mem_equal(bytes, "preserve", sizeof(bytes));
	zassert_ok(fs_close(&file));
}

void check_directory_handles()
{
	struct fs_dir_t handles[CONFIG_FS_LITTLEFS_NUM_DIRS];
	size_t opened = 0;

	for (; opened < ARRAY_SIZE(handles); ++opened) {
		fs_dir_t_init(&handles[opened]);
		if (fs_opendir(&handles[opened], mount_path) != 0) {
			break;
		}
	}
	for (size_t index = 0; index < opened; ++index) {
		zassert_ok(fs_closedir(&handles[index]));
	}
	zassert_equal(opened, ARRAY_SIZE(handles),
		      "A directory handle leaked after allocation failure");
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
	failure_after.store(-1);
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
	failure_after.store(-1);
	zassert_ok(fs_unmount(&mount));
}

} /* namespace */

ZTEST(libcxx_filesystem_alloc, test_noexcept_metadata_reports_allocation_failure)
{
	const auto file = child("file_name_long_enough_to_force_path_storage_allocation");

	create_file(file);
	fs::file_status status;
	uintmax_t size = 0;
	bool exists = true;
	fs::space_info capacity{};

	fail_next([&](std::error_code &ec) { status = fs::status(file, ec); });
	zassert_equal(status.type(), fs::file_type::none);
	fail_next([&](std::error_code &ec) { size = fs::file_size(file, ec); });
	zassert_equal(size, std::numeric_limits<uintmax_t>::max());
	fail_next([&](std::error_code &ec) { exists = fs::exists(file, ec); });
	zassert_false(exists);
	fail_next([&](std::error_code &ec) { capacity = fs::space(file, ec); });
	zassert_equal(capacity.capacity, std::numeric_limits<uintmax_t>::max());
	fail_next([&](std::error_code &ec) { (void)fs::last_write_time(file, ec); });
	fail_next([&](std::error_code &ec) { fs::permissions(file, fs::perms::owner_read, ec); });

	fs::directory_entry entry(file);

	fail_next([&](std::error_code &ec) { entry.refresh(ec); });
	check_file(file);
}

ZTEST(libcxx_filesystem_alloc, test_noexcept_mutations_preserve_files_and_current_directory)
{
	const auto file = child("existing_file_name_long_enough_to_force_allocation");
	const auto target = child("destination_name_long_enough_to_force_allocation");
	const auto directory = child("directory_name_long_enough_to_force_allocation");
	std::error_code ec;

	create_file(file);
	zassert_true(fs::create_directory(directory, ec));
	zassert_false(static_cast<bool>(ec));
	const auto initial_directory = fs::current_path(ec);
	bool removed = true;
	bool created = true;

	fail_next([&](std::error_code &error) { removed = fs::remove(file, error); });
	zassert_false(removed);
	fail_next([&](std::error_code &error) { fs::rename(file, target, error); });
	fail_next([&](std::error_code &error) { fs::resize_file(file, 1, error); });
	fail_next([&](std::error_code &error) { created = fs::create_directory(target, error); });
	zassert_false(created);
	fail_next([&](std::error_code &error) { fs::current_path(directory, error); });

	zassert_true(fs::current_path(ec) == initial_directory);
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::exists(target, ec));
	zassert_false(static_cast<bool>(ec));
	check_file(file);
	/* The current-directory lock must also be usable after a failed allocation. */
	fs::current_path(directory, ec);
	zassert_false(static_cast<bool>(ec));
	zassert_true(fs::current_path(ec) == directory);

	const fs::path relative("relative_file_name_long_enough_to_require_allocation");

	create_file(directory / relative);
	fail_next([&](std::error_code &error) { (void)fs::status(relative, error); });
	zassert_true(fs::current_path(ec) == directory);
	zassert_false(static_cast<bool>(ec));
	check_file(directory / relative);
}

ZTEST(libcxx_filesystem_alloc, test_equivalent_allocation_failure_sweep)
{
	const auto left = child("left_file_name_long_enough_to_force_path_allocation");
	const auto right = child("right_file_name_long_enough_to_force_path_allocation");
	bool reached_success = false;
	size_t failures = 0;

	create_file(left);
	create_file(right);
	for (int position = 0; position < 128; ++position) {
		std::error_code ec;
		bool equivalent = true;

		{
			failure_scope failure(position);

			equivalent = fs::equivalent(left, right, ec);
		}
		if (!failure_hit.load()) {
			zassert_false(static_cast<bool>(ec));
			zassert_false(equivalent);
			reached_success = true;
			break;
		}
		++failures;
		zassert_false(equivalent);
		zassert_true(ec == std::errc::not_enough_memory);
		check_directory_handles();
	}
	zassert_true(reached_success, "Allocation sweep did not finish");
	zassert_true(failures > 2, "Sweep did not exercise later path/mount allocations");
	check_file(left);
	check_file(right);
}

ZTEST(libcxx_filesystem_alloc, test_directory_constructor_allocation_failure_sweep)
{
	const auto directory = child("directory_name_long_enough_to_force_path_allocation");
	const auto entry = directory / "entry_name_long_enough_to_force_an_allocation";
	std::error_code ec;

	zassert_true(fs::create_directory(directory, ec));
	zassert_false(static_cast<bool>(ec));
	create_file(entry);
	bool reached_success = false;
	size_t failures = 0;

	for (int position = 0; position < 128; ++position) {
		bool caught = false;
		bool found = false;

		ec.clear();
		{
			failure_scope failure(position);

			try {
				fs::directory_iterator iterator(directory, ec);

				found = !ec && iterator != fs::directory_iterator();
			} catch (const std::bad_alloc &) {
				caught = true;
			}
		}
		check_directory_handles();
		if (!failure_hit.load()) {
			zassert_false(caught);
			zassert_false(static_cast<bool>(ec));
			zassert_true(found);
			reached_success = true;
			break;
		}
		++failures;
		zassert_true(caught || ec == std::errc::not_enough_memory);
	}
	zassert_true(reached_success, "Allocation sweep did not finish");
	zassert_true(failures > 2, "Constructor allocations were not exercised");
	check_file(entry);
}

ZTEST(libcxx_filesystem_alloc, test_recursive_copy_allocation_failure_sweep)
{
	const auto source = child("source_directory_name_long_enough_to_force_allocation");
	const auto target = child("target_directory_name_long_enough_to_force_allocation");
	const auto entry = source / "file_name_long_enough_to_force_path_allocation";
	std::error_code ec;

	zassert_true(fs::create_directory(source, ec));
	zassert_false(static_cast<bool>(ec));
	create_file(entry);
	bool reached_success = false;
	size_t failures = 0;

	for (int position = 0; position < 128; ++position) {
		fs::remove_all(target, ec);
		zassert_false(static_cast<bool>(ec));
		bool caught = false;

		{
			failure_scope failure(position);

			try {
				fs::copy(source, target, fs::copy_options::recursive, ec);
			} catch (const std::bad_alloc &) {
				caught = true;
			}
		}
		check_directory_handles();
		check_file(entry);
		if (!failure_hit.load()) {
			zassert_false(caught);
			zassert_false(static_cast<bool>(ec));
			check_file(target / entry.filename());
			reached_success = true;
			break;
		}
		++failures;
		zassert_true(caught || ec == std::errc::not_enough_memory);
	}
	zassert_true(reached_success, "Allocation sweep did not finish");
	zassert_true(failures > 2, "Copy traversal allocations were not exercised");
}

ZTEST_SUITE(libcxx_filesystem_alloc, nullptr, setup, before, nullptr, teardown);
