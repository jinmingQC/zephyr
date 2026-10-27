# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Build the runtimes with the application's target flags and C library.
# Nothing in the compiler installation or its sysroot is modified.
include(FetchContent)

if(CONFIG_USERSPACE)
  message(FATAL_ERROR "The libc++ PoC currently requires supervisor-only builds")
endif()
if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "14")
    OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17"))
  message(FATAL_ERROR "libc++ 19 requires GCC 14 or Clang 17 or newer")
endif()

set(LIBCXX_SOURCE_DIR "" CACHE PATH "Path to unpacked libc++ 19.1.7 sources")
set(LIBCXXABI_SOURCE_DIR "" CACHE PATH "Path to unpacked libc++abi 19.1.7 sources")

if(NOT LIBCXX_SOURCE_DIR)
  FetchContent_Declare(libcxx_source
    URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/libcxx-19.1.7.src.tar.xz"
    URL_HASH "SHA256=b736109650ffc750dbdc506483347b3713ded9d0300f48432b820ad66b6a9052"
  )
  FetchContent_GetProperties(libcxx_source)
  if(NOT libcxx_source_POPULATED)
    FetchContent_Populate(libcxx_source)
  endif()
  set(LIBCXX_SOURCE_DIR "${libcxx_source_SOURCE_DIR}")
endif()

if(NOT LIBCXXABI_SOURCE_DIR)
  FetchContent_Declare(libcxxabi_source
    URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/libcxxabi-19.1.7.src.tar.xz"
    URL_HASH "SHA256=4d25125109e3febe1393e9e30f93153333d82b48ce7bed637bd2daf52bf40172"
  )
  FetchContent_GetProperties(libcxxabi_source)
  if(NOT libcxxabi_source_POPULATED)
    FetchContent_Populate(libcxxabi_source)
  endif()
  set(LIBCXXABI_SOURCE_DIR "${libcxxabi_source_SOURCE_DIR}")
endif()

file(STRINGS "${LIBCXX_SOURCE_DIR}/include/__config" libcxx_version
  REGEX "^# *define _LIBCPP_VERSION "
)
if(NOT libcxx_version MATCHES "190107$")
  message(FATAL_ERROR "This libc++ PoC requires libc++ 19.1.7 sources")
endif()
if(CONFIG_LIBCXX_THREAD_PRIORITY GREATER_EQUAL CONFIG_NUM_PREEMPT_PRIORITIES)
  message(FATAL_ERROR "LIBCXX_THREAD_PRIORITY must be less than NUM_PREEMPT_PRIORITIES")
endif()

set(_LIBCPP_ABI_VERSION 1)
set(_LIBCPP_ABI_NAMESPACE "__1")
set(_LIBCPP_HAS_THREAD_API_EXTERNAL ON)
set(_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS ON)
set(_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS ON)
if(CONFIG_LIBCXX_FILESYSTEM)
  if(NOT CONFIG_LIBCXX_FILESYSTEM_TEMP_DIR MATCHES "^/")
    message(FATAL_ERROR "LIBCXX_FILESYSTEM_TEMP_DIR must be an absolute path")
  endif()
  set(_LIBCPP_HAS_NO_FILESYSTEM OFF)
else()
  set(_LIBCPP_HAS_NO_FILESYSTEM ON)
endif()
if(CONFIG_LIBCXX_RANDOM_DEVICE)
  set(_LIBCPP_HAS_NO_RANDOM_DEVICE OFF)
else()
  set(_LIBCPP_HAS_NO_RANDOM_DEVICE ON)
endif()
if(CONFIG_LIBCXX_STREAMS)
  set(_LIBCPP_HAS_NO_LOCALIZATION OFF)
else()
  set(_LIBCPP_HAS_NO_LOCALIZATION ON)
endif()
set(_LIBCPP_HAS_NO_UNICODE ON)
set(_LIBCPP_HAS_NO_WIDE_CHARACTERS ON)
set(_LIBCPP_HAS_NO_STD_MODULES ON)
set(_LIBCPP_HAS_NO_TIME_ZONE_DATABASE ON)
set(_LIBCPP_PSTL_BACKEND_SERIAL ON)
set(_LIBCPP_HARDENING_MODE_DEFAULT "_LIBCPP_HARDENING_MODE_NONE")
configure_file("${LIBCXX_SOURCE_DIR}/include/__config_site.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/__config_site" @ONLY
)
configure_file("${LIBCXX_SOURCE_DIR}/vendor/llvm/default_assertion_handler.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/__assertion_handler" COPYONLY
)

# -nostdinc++ is needed with GCC as well as Clang. Only C++ translation
# units see libc++'s headers, before the toolchain's C library headers.
zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>)
# Use the existing GNU-intrinsic backend consistently in Clang runtime and apps.
# Clang 19's C11 lowering pads a 12-byte aggregate to 16 bytes and can omit
# expected-value copy-back after a failed compare-exchange on RISC-V.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  zephyr_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_LIBCPP_HAS_GCC_ATOMIC_IMP>)
endif()

foreach(include_dir
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${LIBCXX_SOURCE_DIR}/include"
    "${LIBCXXABI_SOURCE_DIR}/include"
)
  target_include_directories(zephyr_interface SYSTEM BEFORE INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:${include_dir}>"
  )
endforeach()

include(headers.cmake)
include(gcc_headers.cmake)

zephyr_linker_sources(ROM_SECTIONS libcxx.ld)

zephyr_library_named(libcxx_source_runtime)
zephyr_library_sources(
  threading.cpp
  atomics.c
)
zephyr_library_sources_ifdef(CONFIG_LIBCXX_RANDOM_DEVICE random_device.cpp)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  zephyr_library_sources(string_destructor.cpp)
endif()
# LLVM builds its runtime in C++23 mode; applications retain STD_CPP17.
set_property(TARGET libcxx_source_runtime PROPERTY CXX_SCAN_FOR_MODULES OFF)
zephyr_library_include_directories("${LIBCXX_SOURCE_DIR}/src")
zephyr_library_compile_definitions(
  _LIBCPP_BUILDING_LIBRARY
  _LIBCXXABI_BUILDING_LIBRARY
  _LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS
  HAVE___CXA_THREAD_ATEXIT_IMPL
)
# The ABI is provided by the libc++abi sources below.
set_property(TARGET libcxx_source_runtime APPEND PROPERTY COMPILE_DEFINITIONS
  LIBCXX_BUILDING_LIBCXXABI
)

set(libcxx_sources
  algorithm.cpp
  any.cpp
  atomic.cpp
  barrier.cpp
  bind.cpp
  call_once.cpp
  charconv.cpp
  chrono.cpp
  condition_variable.cpp
  condition_variable_destructor.cpp
  error_category.cpp
  exception.cpp
  expected.cpp
  functional.cpp
  future.cpp
  hash.cpp
  legacy_pointer_safety.cpp
  memory.cpp
  memory_resource.cpp
  mutex.cpp
  mutex_destructor.cpp
  new_handler.cpp
  new_helpers.cpp
  optional.cpp
  random_shuffle.cpp
  ryu/d2fixed.cpp
  ryu/d2s.cpp
  ryu/f2s.cpp
  shared_mutex.cpp
  stdexcept.cpp
  string.cpp
  system_error.cpp
  thread.cpp
  typeinfo.cpp
  valarray.cpp
  variant.cpp
  vector.cpp
  verbose_abort.cpp
)
if(CONFIG_LIBCXX_FILESYSTEM)
  list(APPEND libcxx_sources
    filesystem/filesystem_clock.cpp
    filesystem/filesystem_error.cpp
    filesystem/path.cpp
  )
  zephyr_library_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBCXX_SOURCE_DIR}/src/filesystem"
  )
  zephyr_library_sources(
    filesystem.cpp
    filesystem_copy.cpp
    filesystem_directory.cpp
  )

  # Retain LLVM's iterator traversal and ABI while replacing its POSIX-only
  # helper header. The pinned source and its license remain in the build tree.
  file(READ "${LIBCXX_SOURCE_DIR}/src/filesystem/directory_iterator.cpp" iterator_source)
  string(REPLACE
    "#include \"file_descriptor.h\""
    "#include \"zephyr_directory_support.hpp\""
    native_iterator_source "${iterator_source}"
  )
  if(native_iterator_source STREQUAL iterator_source)
    message(FATAL_ERROR "The pinned libc++ directory iterator include has changed")
  endif()
  include(directory_exceptions.cmake)
  set(iterator_path "${CMAKE_CURRENT_BINARY_DIR}/directory_iterator.cpp")
  file(CONFIGURE OUTPUT "${iterator_path}" CONTENT "${native_iterator_source}" @ONLY)
  zephyr_library_sources("${iterator_path}")
  set_source_files_properties("${iterator_path}" PROPERTIES COMPILE_DEFINITIONS
    "opendir=z_libcxx_opendir;readdir=z_libcxx_readdir;closedir=z_libcxx_closedir"
  )
endif()

if(CONFIG_LIBCXX_STREAMS)
  include(streams.cmake)
endif()

foreach(source IN LISTS libcxx_sources)
  zephyr_library_sources("${LIBCXX_SOURCE_DIR}/src/${source}")
endforeach()

set(libcxxabi_sources
  abort_message.cpp
  cxa_aux_runtime.cpp
  cxa_default_handlers.cpp
  cxa_demangle.cpp
  cxa_exception_storage.cpp
  cxa_guard.cpp
  cxa_handlers.cpp
  cxa_thread_atexit.cpp
  cxa_vector.cpp
  cxa_virtual.cpp
  fallback_malloc.cpp
  stdlib_exception.cpp
  stdlib_new_delete.cpp
  stdlib_stdexcept.cpp
  stdlib_typeinfo.cpp
)
if(CONFIG_CPP_EXCEPTIONS)
  list(APPEND libcxxabi_sources
    cxa_exception.cpp
    cxa_personality.cpp
  )
else()
  list(APPEND libcxxabi_sources cxa_noexception.cpp)
endif()
if(CONFIG_CPP_EXCEPTIONS OR CONFIG_CPP_RTTI)
  list(APPEND libcxxabi_sources private_typeinfo.cpp)
endif()
foreach(source IN LISTS libcxxabi_sources)
  zephyr_library_sources("${LIBCXXABI_SOURCE_DIR}/src/${source}")
endforeach()

# Source options follow Zephyr's interface -std=c++17 flag in the command.
# Runtime classes need consistent typeinfo for exception matching, including
# library classes used as bases of application exceptions. Application RTTI
# remains independently selectable.
get_target_property(runtime_sources libcxx_source_runtime SOURCES)
foreach(source IN LISTS runtime_sources)
  if(source MATCHES "\\.cpp$")
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS "-std=c++23")
    if(CONFIG_CPP_EXCEPTIONS OR CONFIG_CPP_RTTI)
      set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS "-frtti")
    endif()
  endif()
endforeach()

# GCC 14 can omit the constexpr string destructor's exported C++23 definition.
# Instantiate that same header implementation in C++17 mode for ABI consumers.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_property(SOURCE string_destructor.cpp APPEND PROPERTY COMPILE_OPTIONS "-std=c++17")
endif()

# Upstream runtime sources are not written against Zephyr's extra C warning
# profile. Keep these exceptions off the adapter and application sources.
foreach(source IN LISTS libcxx_sources)
  set_property(SOURCE "${LIBCXX_SOURCE_DIR}/src/${source}" APPEND PROPERTY
    COMPILE_OPTIONS "-Wno-shadow;-Wno-double-promotion"
  )
endforeach()
foreach(source IN LISTS libcxxabi_sources)
  set_property(SOURCE "${LIBCXXABI_SOURCE_DIR}/src/${source}" APPEND PROPERTY
    COMPILE_OPTIONS "-Wno-shadow;-Wno-double-promotion"
  )
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CONFIG_CPP_EXCEPTIONS)
    # GCC loses the native-exception guard across the upstream catch block.
    set_property(SOURCE "${LIBCXXABI_SOURCE_DIR}/src/cxa_personality.cpp" APPEND PROPERTY
      COMPILE_OPTIONS "-Wno-maybe-uninitialized"
    )
  endif()
  if(CONFIG_LIBCXX_STREAMS)
    set_property(SOURCE "${LIBCXX_SOURCE_DIR}/src/locale.cpp" APPEND PROPERTY
      COMPILE_OPTIONS "-Wno-attributes"
    )
  endif()
  # GCC warns about LLVM's always_inline annotation without an inline keyword.
  set_property(SOURCE "${LIBCXX_SOURCE_DIR}/src/charconv.cpp" APPEND PROPERTY
    COMPILE_OPTIONS "-Wno-attributes"
  )
  # Ryu guarantees a nonzero mantissa before the bit scan; GCC cannot infer it.
  set_property(SOURCE "${LIBCXX_SOURCE_DIR}/src/ryu/d2s.cpp" APPEND PROPERTY
    COMPILE_OPTIONS "-Wno-maybe-uninitialized"
  )
endif()
# Upstream returns the standard-permitted unknown value (0) on this platform.
set_property(SOURCE "${LIBCXX_SOURCE_DIR}/src/thread.cpp" APPEND PROPERTY
  COMPILE_OPTIONS "-Wno-cpp"
)

if(CONFIG_CPP_EXCEPTIONS)
  include(unwind.cmake)
endif()
