# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Keep a contiguous string representable by its iterator difference_type.
# The pinned release's little-endian layout otherwise admits capacities above
# PTRDIFF_MAX, which GCC rejects when optimizing its saturated growth path.
file(READ "${LIBCXX_SOURCE_DIR}/include/string" string_source)
string(REPLACE
  "size_type __m = __alloc_traits::max_size(__alloc());"
  "size_type __m = std::min(__alloc_traits::max_size(__alloc()),\n        static_cast<size_type>(std::numeric_limits<difference_type>::max()));"
  native_string_source "${string_source}"
)
if(native_string_source STREQUAL string_source)
  message(FATAL_ERROR "The pinned libc++ string capacity expression has changed")
endif()
set(portable_headers_dir "${CMAKE_CURRENT_BINARY_DIR}/portable/include")
file(CONFIGURE OUTPUT "${portable_headers_dir}/string"
  CONTENT "${native_string_source}" @ONLY
)
target_include_directories(zephyr_interface SYSTEM BEFORE INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:${portable_headers_dir}>"
)
