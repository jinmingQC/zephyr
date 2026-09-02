# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kasan_global)

target_sources(app PRIVATE
  src/main.c
  instrumented/globals_a.c
  instrumented/globals_b.c
)
target_include_directories(app PRIVATE src)

# Keep the test harness and report override uninstrumented. Instrumenting two
# translation units also exercises descriptor registration and index ordering.
zephyr_kasan_enable_directory(instrumented)
