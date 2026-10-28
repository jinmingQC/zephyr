# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cpp_runtime_userspace_failfast)

set(CPP_RUNTIME_FAILURE_CASE "1" CACHE STRING "One failure scenario per fresh image")
if(NOT CPP_RUNTIME_FAILURE_CASE MATCHES "^[123]$")
  message(FATAL_ERROR "CPP_RUNTIME_FAILURE_CASE must be 1, 2, or 3")
endif()
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE include)
target_compile_definitions(app PRIVATE CPP_RUNTIME_FAILURE_CASE=${CPP_RUNTIME_FAILURE_CASE})
zephyr_link_libraries(-Wl,--wrap=abort)
