# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cpp_runtime_thread_userspace)

zephyr_include_directories(include)
target_sources(app PRIVATE src/main.cpp src/user_test_helpers.c)

# Compiler-generated guard variables are ordinary writable application data.
# This application archive uses the same partition mapping as
# zephyr_library_app_memory(), without creating a kernel-mode Zephyr library.
add_library(cpp_userspace_static_test STATIC src/static_local.cpp)
target_link_libraries(cpp_userspace_static_test PUBLIC zephyr_interface)
add_dependencies(cpp_userspace_static_test zephyr_generated_headers)
target_link_libraries(app PUBLIC cpp_userspace_static_test)
set_property(TARGET zephyr_property_target APPEND PROPERTY COMPILE_OPTIONS
  "-l" "$<TARGET_FILE_NAME:cpp_userspace_static_test>" "ztest_mem_partition"
)
