# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Build the runtimes with the application's target flags and C library.
# Nothing in the compiler installation or its sysroot is modified.
include(FetchContent)

if(NOT CONFIG_CPP_RUNTIME)
  return()
endif()

if(NOT CONFIG_EXTERNAL_MODULE_LIBCPP OR NOT CONFIG_PICOLIBC)
  message(FATAL_ERROR "The thread-only profile requires EXTERNAL_MODULE_LIBCPP and PICOLIBC")
endif()

if(CONFIG_CPP_EXCEPTIONS OR CONFIG_CPP_RTTI)
  message(FATAL_ERROR "The thread-only profile requires exceptions and RTTI disabled")
endif()

if((CONFIG_CPP_RUNTIME_NATIVE AND CONFIG_CPP_RUNTIME_POSIX)
    OR (NOT CONFIG_CPP_RUNTIME_NATIVE AND NOT CONFIG_CPP_RUNTIME_POSIX))
  message(FATAL_ERROR "Exactly one runtime backend is required; POSIX needs POSIX_SYSTEM_INTERFACES")
endif()
if(CONFIG_STD_CPP_VERSION LESS 201703)
  message(FATAL_ERROR "The thread-only profile requires C++17 or newer")
endif()
if(NOT CONFIG_POSIX_TIMERS OR NOT CONFIG_POSIX_MONOTONIC_CLOCK)
  message(FATAL_ERROR "The thread-only profile requires POSIX_TIMERS and POSIX_MONOTONIC_CLOCK")
endif()

if(CONFIG_USERSPACE)
  message(FATAL_ERROR "The libc++ thread PoC currently requires supervisor-only builds")
endif()
if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13")
    OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16"))
  message(FATAL_ERROR "libc++ 18 requires GCC 13 or Clang 16 or newer")
endif()

set(LIBCXX_SOURCE_DIR "" CACHE PATH "Path to unpacked libc++ 18.1.8 sources")
set(LIBCXXABI_SOURCE_DIR "" CACHE PATH "Path to unpacked libc++abi 18.1.8 sources")

if(NOT LIBCXX_SOURCE_DIR)
  FetchContent_Declare(libcxx_source
    URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/libcxx-18.1.8.src.tar.xz"
    URL_HASH "SHA256=bdecf90be0072bc720fd5c9c8ab061cdb197edd0c8ad3e170dc3e6bfaa49f388"
  )
  FetchContent_GetProperties(libcxx_source)
  if(NOT libcxx_source_POPULATED)
    FetchContent_Populate(libcxx_source)
  endif()
  set(LIBCXX_SOURCE_DIR "${libcxx_source_SOURCE_DIR}")
endif()

if(NOT LIBCXXABI_SOURCE_DIR)
  FetchContent_Declare(libcxxabi_source
    URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/libcxxabi-18.1.8.src.tar.xz"
    URL_HASH "SHA256=256c30d724eeb72713bc08ae1692f53aaf4ebe8a1d662c92bf59e69d6c53dce9"
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
if(NOT libcxx_version MATCHES "180100$")
  message(FATAL_ERROR "This libc++ thread PoC requires libc++ 18.1.8 sources")
endif()
if(CONFIG_CPP_RUNTIME_THREAD_PRIORITY GREATER_EQUAL CONFIG_NUM_PREEMPT_PRIORITIES)
  message(FATAL_ERROR "CPP_RUNTIME_THREAD_PRIORITY must be less than NUM_PREEMPT_PRIORITIES")
endif()

set(_LIBCPP_ABI_VERSION 1)
set(_LIBCPP_ABI_NAMESPACE "__1")
if(CONFIG_CPP_RUNTIME_NATIVE)
  set(_LIBCPP_HAS_THREAD_API_EXTERNAL ON)
elseif(CONFIG_CPP_RUNTIME_POSIX)
  set(_LIBCPP_HAS_THREAD_API_PTHREAD ON)
endif()
set(_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS ON)
set(_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS ON)
set(_LIBCPP_HAS_NO_FILESYSTEM ON)
set(_LIBCPP_HAS_NO_RANDOM_DEVICE ON)
set(_LIBCPP_HAS_NO_LOCALIZATION ON)
set(_LIBCPP_HAS_NO_WIDE_CHARACTERS ON)
set(_LIBCPP_HAS_NO_STD_MODULES ON)
set(_LIBCPP_HAS_NO_TIME_ZONE_DATABASE ON)
set(_LIBCPP_PSTL_CPU_BACKEND_SERIAL ON)
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

zephyr_linker_sources(ROM_SECTIONS libcxx.ld)

zephyr_library_named(libcxx_source_runtime)
if(CONFIG_CPP_RUNTIME_NATIVE)
  zephyr_library_sources(adapters/native_threading.cpp)
else()
  zephyr_library_sources(adapters/posix_runtime.c)
  zephyr_link_libraries(
    -Wl,--wrap=pthread_create
    -Wl,--wrap=pthread_self
    -Wl,--wrap=pthread_exit
  )
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
  cxa_noexception.cpp
  cxa_thread_atexit.cpp
  cxa_vector.cpp
  cxa_virtual.cpp
  fallback_malloc.cpp
  stdlib_exception.cpp
  stdlib_new_delete.cpp
  stdlib_stdexcept.cpp
  stdlib_typeinfo.cpp
)
foreach(source IN LISTS libcxxabi_sources)
  zephyr_library_sources("${LIBCXXABI_SOURCE_DIR}/src/${source}")
endforeach()

# Source options follow Zephyr's interface -std=c++17 flag in the command.
get_target_property(runtime_sources libcxx_source_runtime SOURCES)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17")
  set(runtime_standard "-std=c++2b")
else()
  set(runtime_standard "-std=c++23")
endif()
foreach(source IN LISTS runtime_sources)
  if(source MATCHES "[.]cpp$")
    set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "${runtime_standard}")
  endif()
endforeach()

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

# LLVM 18 explicitly instantiates this deprecated compatibility specialization.
# Limit the exception to upstream string.cpp, retaining -Werror for our code.
set_property(SOURCE "${LIBCXX_SOURCE_DIR}/src/string.cpp" APPEND PROPERTY
  COMPILE_OPTIONS "-Wno-deprecated-declarations"
)
