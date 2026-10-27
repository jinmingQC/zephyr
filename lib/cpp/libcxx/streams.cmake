# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

list(APPEND libcxx_sources
  fstream.cpp
  ios.cpp
  ios.instantiations.cpp
  iostream.cpp
  locale.cpp
  ostream.cpp
  regex.cpp
  strstream.cpp
)
zephyr_library_sources(streams.cpp)

# The libc++ newlib locale port uses POSIX locale APIs and GNU stdio helpers.
# Picolibc exposes its real funopen FILE implementation under this feature set.
zephyr_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_GNU_SOURCE>)

# Retain the upstream filebuf implementation and adapt the open boundary.
# The generated header keeps its original license and is shared by runtime and
# application translation units.
file(READ "${LIBCXX_SOURCE_DIR}/include/fstream" fstream_source)
string(REPLACE
  "#include <__config>"
  "#include <__config>\n#include <__zephyr_stdio.h>"
  native_fstream_source "${fstream_source}"
)
string(REPLACE
  "return __do_open(fopen(__s, __mdstr), __mode);"
  "return __do_open(z_libcxx_fopen(__s, __mdstr), __mode);"
  adapted_fstream_source "${native_fstream_source}"
)
if(adapted_fstream_source STREQUAL native_fstream_source)
  message(FATAL_ERROR "The pinned libc++ fstream open expression has changed")
endif()
set(streams_include_dir "${CMAKE_CURRENT_BINARY_DIR}/streams/include")
file(CONFIGURE OUTPUT "${streams_include_dir}/fstream"
  CONTENT "${adapted_fstream_source}" @ONLY
)
target_include_directories(zephyr_interface SYSTEM BEFORE INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:${streams_include_dir}>"
)

# GCC cannot infer that successful formatted extraction initializes the value.
# Initialize the pinned helper instead of suppressing application diagnostics.
file(READ "${LIBCXX_SOURCE_DIR}/include/iomanip" iomanip_source)
string(REPLACE "_CharT __c;" "_CharT __c{};" adapted_iomanip_source "${iomanip_source}")
if(adapted_iomanip_source STREQUAL iomanip_source)
  message(FATAL_ERROR "The pinned libc++ quoted-input helper has changed")
endif()
file(CONFIGURE OUTPUT "${streams_include_dir}/iomanip"
  CONTENT "${adapted_iomanip_source}" @ONLY
)
