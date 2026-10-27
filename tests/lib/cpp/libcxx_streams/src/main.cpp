/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/ztest.h>

namespace fs = std::filesystem;

BUILD_ASSERT(__cplusplus == 201703L);

namespace
{

constexpr const char *mount_path = "/lfs";
constexpr const char *test_path = "/lfs/streams";
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

std::string payload(size_t size = 8193)
{
	std::string result(size, '\0');

	for (size_t index = 0; index < size; ++index) {
		result[index] = static_cast<char>((index * 37U + 3U) & 0xffU);
	}
	return result;
}

void write_text(const fs::path &path, const std::string &text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);

	zassert_true(output.is_open());
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	zassert_true(output.good());
	output.close();
	zassert_false(output.fail());
}

std::string read_text(const fs::path &path)
{
	std::ifstream input(path, std::ios::binary);

	zassert_true(input.is_open());
	std::string result(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});

	zassert_false(input.bad());
	input.close();
	zassert_false(input.bad());
	return result;
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

ZTEST(libcxx_streams, test_stringstream_integer_round_trip)
{
	std::stringstream stream;
	const long long original = -9223372036854775807LL;

	stream << original << ' ' << std::hex << 0x5a39;
	long long value = 0;
	unsigned int hex = 0;

	/* Input and output share the stream's persistent basefield flags. */
	zassert_true(stream.str() == "-9223372036854775807 5a39");
	stream >> std::dec >> value >> std::hex >> hex;
	zassert_false(stream.fail());
	zassert_equal(value, original);
	zassert_equal(hex, 0x5a39);
}

ZTEST(libcxx_streams, test_stringstream_floating_point)
{
	std::ostringstream output;

	output << std::fixed << std::setprecision(3) << 1234.625;
	zassert_true(output.str() == "1234.625");

	std::istringstream input(output.str());
	double value = 0;

	input >> value;
	zassert_false(input.fail());
	zassert_true(std::abs(value - 1234.625) < 0.0001);
}

ZTEST(libcxx_streams, test_stream_formatting_and_quotes)
{
	std::ostringstream output;
	const std::string original = "name with \"quotes\"";

	output << std::setfill('0') << std::setw(5) << 42 << ' ' << std::boolalpha << true << ' '
	       << std::quoted(original);
	zassert_true(output.str() == "00042 true \"name with \\\"quotes\\\"\"");

	std::istringstream input(output.str());
	int number = 0;
	bool boolean = false;
	std::string decoded;

	input >> number >> std::boolalpha >> boolean >> std::quoted(decoded);
	zassert_false(input.fail());
	zassert_equal(number, 42);
	zassert_true(boolean);
	zassert_true(decoded == original);
}

ZTEST(libcxx_streams, test_stream_fail_state_and_recovery)
{
	std::istringstream input("invalid 27");
	int number = 0;

	input >> number;
	zassert_true(input.fail());
	input.clear();
	std::string ignored;
	input >> ignored >> number;
	zassert_false(input.fail());
	zassert_equal(number, 27);
	zassert_true(ignored == "invalid");
}

ZTEST(libcxx_streams, test_classic_locale_facets)
{
	const std::locale locale = std::locale::classic();
	const auto &punctuation = std::use_facet<std::numpunct<char>>(locale);
	const auto &classification = std::use_facet<std::ctype<char>>(locale);

	zassert_true(locale.name() == "C");
	zassert_equal(punctuation.decimal_point(), '.');
	zassert_true(classification.is(std::ctype_base::alpha, 'a'));
	zassert_true(classification.is(std::ctype_base::digit, '7'));
	zassert_equal(classification.toupper('q'), 'Q');
	zassert_true(std::locale("C") == locale);
}

ZTEST(libcxx_streams, test_console_streams)
{
	zassert_not_null(std::cout.rdbuf());
	zassert_not_null(std::cerr.rdbuf());
	zassert_not_null(std::clog.rdbuf());
	zassert_true(std::cin.tie() == &std::cout);

	std::cout << "libc++ stream output: " << 17 << '\n' << std::flush;
	zassert_true(std::cout.good());
	std::clog << "libc++ log output" << '\n' << std::flush;
	zassert_true(std::clog.good());
}

ZTEST(libcxx_streams, test_regex_capture_and_replace)
{
	const std::regex expression("([a-z]+)=([0-9]+)");
	const std::string input = "value=123";
	std::smatch result;

	zassert_true(std::regex_match(input, result, expression));
	zassert_equal(result.size(), 3);
	zassert_true(result[1].str() == "value");
	zassert_true(result[2].str() == "123");
	zassert_true(std::regex_replace(input, expression, "$2:$1") == "123:value");
}

ZTEST(libcxx_streams, test_fstream_binary_round_trip)
{
	const auto contents = payload();

	write_text(child("binary"), contents);
	zassert_true(read_text(child("binary")) == contents);

	std::error_code ec;

	zassert_equal(fs::file_size(child("binary"), ec), contents.size());
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_streams, test_fstream_native_interoperation)
{
	write_text(child("file"), "C++ file contents");
	struct fs_file_t native;
	char contents[32]{};

	fs_file_t_init(&native);
	zassert_ok(fs_open(&native, child("file").c_str(), FS_O_READ));
	zassert_equal(fs_read(&native, contents, sizeof(contents)), 17);
	zassert_mem_equal(contents, "C++ file contents", 17);
	zassert_ok(fs_close(&native));
}

ZTEST(libcxx_streams, test_fstream_current_path)
{
	std::error_code ec;

	fs::current_path(test_path, ec);
	zassert_false(static_cast<bool>(ec));
	write_text("relative", "shared current directory");
	zassert_true(read_text(child("relative")) == "shared current directory");
	zassert_true(fs::exists("relative", ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_streams, test_fstream_append_and_truncate)
{
	write_text(child("file"), "one");
	{
		std::ofstream output(child("file"), std::ios::app);

		output << "-two";
		output.close();
		zassert_false(output.fail());
	}
	zassert_true(read_text(child("file")) == "one-two");
	write_text(child("file"), "new");
	zassert_true(read_text(child("file")) == "new");
}

ZTEST(libcxx_streams, test_fstream_update_seek_and_tell)
{
	write_text(child("file"), "0123456789");
	std::fstream stream(child("file"), std::ios::in | std::ios::out | std::ios::binary);

	zassert_true(stream.is_open());
	stream.seekp(4);
	zassert_equal(static_cast<std::streamoff>(stream.tellp()), 4);
	stream.write("AB", 2);
	stream.flush();
	zassert_true(stream.good());
	stream.seekg(-3, std::ios::end);
	char suffix[3]{};
	stream.read(suffix, sizeof(suffix));
	zassert_true(stream.good());
	zassert_mem_equal(suffix, "789", sizeof(suffix));
	stream.close();
	zassert_false(stream.fail());
	zassert_true(read_text(child("file")) == "0123AB6789");
}

ZTEST(libcxx_streams, test_fstream_seek_limits_preserve_contents)
{
	write_text(child("file"), "abcd");
	std::fstream stream(child("file"), std::ios::in | std::ios::out | std::ios::binary);

	stream.seekp(1);
	zassert_true(stream.good());
	stream.seekp(-1);
	zassert_true(stream.fail(), "Negative absolute offset was accepted");
	stream.clear();
	zassert_equal(static_cast<std::streamoff>(stream.tellp()), 1);

	const auto largest = static_cast<std::streamoff>(LFS_FILE_MAX);

	stream.seekp(largest);
	zassert_true(stream.good());
	zassert_equal(static_cast<std::streamoff>(stream.tellp()), largest);
	stream.seekp(1, std::ios::cur);
	zassert_true(stream.fail(), "Seek beyond the native file limit was accepted");
	stream.clear();
	zassert_equal(static_cast<std::streamoff>(stream.tellp()), largest);
	stream.seekp(1);
	stream.put('X');
	stream.close();
	zassert_false(stream.fail());
	zassert_true(read_text(child("file")) == "aXcd");
}

ZTEST(libcxx_streams, test_fstream_flush_visibility)
{
	std::ofstream output(child("file"), std::ios::binary);

	output << "visible before close" << std::flush;
	zassert_true(output.good());
	zassert_true(read_text(child("file")) == "visible before close");
	output.close();
	zassert_false(output.fail());
}

ZTEST(libcxx_streams, test_fstream_ate_and_putback)
{
	write_text(child("file"), "abcd");
	std::ifstream input(child("file"), std::ios::ate | std::ios::binary);

	zassert_equal(static_cast<std::streamoff>(input.tellg()), 4);
	input.seekg(0);
	zassert_equal(input.get(), 'a');
	zassert_equal(input.peek(), 'b');
	input.unget();
	zassert_equal(input.get(), 'a');
	zassert_equal(input.get(), 'b');
	zassert_true(input.good());
}

ZTEST(libcxx_streams, test_fstream_move_and_reopen)
{
	std::ofstream original(child("one"));

	original << "first";
	std::ofstream moved(std::move(original));

	zassert_false(original.is_open());
	zassert_true(moved.is_open());
	moved << "-moved";
	moved.close();
	zassert_false(moved.fail());
	moved.open(child("two"));
	moved << "second";
	moved.close();
	zassert_false(moved.fail());
	zassert_true(read_text(child("one")) == "first-moved");
	zassert_true(read_text(child("two")) == "second");
}

ZTEST(libcxx_streams, test_fstream_open_errors_preserve_files)
{
	write_text(child("existing"), "preserved");
	std::ifstream missing(child("missing"));
	std::fstream missing_update(child("missing-update"), std::ios::in | std::ios::out);
	std::ofstream missing_parent(child("missing-parent/file"));
	std::ofstream trailing_separator(child("missing-file/"));
	std::ofstream file_parent(child("existing/child"));

	zassert_true(missing.fail());
	zassert_true(missing_update.fail());
	zassert_true(missing_parent.fail());
	zassert_true(trailing_separator.fail());
	zassert_true(file_parent.fail());
	zassert_true(read_text(child("existing")) == "preserved");

	std::error_code ec;

	zassert_false(fs::exists(child("missing-update"), ec));
	zassert_false(static_cast<bool>(ec));
	zassert_false(fs::exists(child("missing-file"), ec));
	zassert_false(static_cast<bool>(ec));
}

ZTEST(libcxx_streams, test_fstream_close_releases_native_handles)
{
	for (size_t index = 0; index < CONFIG_FS_LITTLEFS_NUM_FILES * 3U; ++index) {
		write_text(child("reused"), "reuse");
		zassert_true(read_text(child("reused")) == "reuse");
	}
}

ZTEST(libcxx_streams, test_concurrent_streams_and_locale)
{
	std::array<std::thread, 4> workers;
	std::atomic<unsigned int> errors{0};

	for (size_t index = 0; index < workers.size(); ++index) {
		workers[index] = std::thread([index, &errors] {
			const auto path = child(("worker" + std::to_string(index)).c_str());

			for (size_t iteration = 0; iteration < 5; ++iteration) {
				std::ostringstream formatted;
				formatted.imbue(std::locale::classic());
				formatted << index << ':' << std::fixed << std::setprecision(2)
					  << 1.25;
				const auto expected = formatted.str();
				{
					std::ofstream output(path);

					output << expected;
					output.close();
					if (output.fail()) {
						++errors;
						return;
					}
				}
				std::ifstream input(path);
				std::string observed;

				input >> observed;
				if (input.fail() || observed != expected) {
					++errors;
					return;
				}
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	zassert_equal(errors.load(), 0);
}

ZTEST(libcxx_streams, test_fstream_disk_full_reports_failure_and_closes)
{
	std::array<char, 4096> block{};
	std::ofstream output(child("full"), std::ios::binary);

	zassert_true(output.is_open());
	for (size_t index = 0; index < 64 && output.good(); ++index) {
		output.write(block.data(), block.size());
		output.flush();
	}
	zassert_true(output.fail(), "Full disk did not set the stream failure state");
	output.close();
	zassert_false(output.is_open(), "Failed close retained the native file handle");

	std::error_code ec;

	zassert_true(fs::remove(child("full"), ec));
	zassert_false(static_cast<bool>(ec));
	write_text(child("recovered"), "recovered");
	zassert_true(read_text(child("recovered")) == "recovered");
}

ZTEST(libcxx_streams, test_fstream_exception_mask)
{
	std::ifstream input;

	zassert_equal(input.exceptions(), std::ios::goodbit);
#if defined(CONFIG_CPP_EXCEPTIONS)
	input.exceptions(std::ios::failbit | std::ios::badbit);
	bool caught = false;

	try {
		input.open(child("missing"));
	} catch (const std::ios_base::failure &) {
		caught = true;
	}
	zassert_true(caught, "Failed open did not propagate ios_base::failure");
	input.exceptions(std::ios::goodbit);
#else
	input.open(child("missing"));
#endif
	zassert_true(input.fail());
	zassert_false(input.is_open());
}

ZTEST(libcxx_streams, test_stringstream_exception_mask)
{
	std::istringstream input("invalid");
	int value = 0;

	zassert_equal(input.exceptions(), std::ios::goodbit);
#if defined(CONFIG_CPP_EXCEPTIONS)
	input.exceptions(std::ios::failbit);
	bool caught = false;

	try {
		input >> value;
	} catch (const std::ios_base::failure &) {
		caught = true;
	}
	zassert_true(caught, "Invalid extraction did not propagate ios_base::failure");
	input.exceptions(std::ios::goodbit);
#else
	input >> value;
#endif
	zassert_true(input.fail());
}

ZTEST_SUITE(libcxx_streams, nullptr, setup, before, nullptr, teardown);
