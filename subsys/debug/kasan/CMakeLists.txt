# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(kasan.c)
zephyr_library_sources_ifdef(CONFIG_KASAN_GLOBAL kasan_global.c)

zephyr_library_compile_definitions_ifdef(CONFIG_SYS_HEAP_KASAN_EXTENSIONS
  _POSIX_C_SOURCE=200809L
  _GNU_SOURCE
)

get_property(_no_kasan TARGET compiler PROPERTY no_kasan)
zephyr_library_compile_options(${_no_kasan})

if(CONFIG_KASAN_GLOBAL)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "CONFIG_KASAN_GLOBAL is only supported with GCC and Clang")
  endif()
  get_property(_kasan_global TARGET compiler PROPERTY kasan_global)
  if(NOT _kasan_global)
    message(FATAL_ERROR "The selected compiler does not provide Global KASAN flags")
  endif()
endif()
