# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  return()
endif()

set(gcc_include_dir "${CMAKE_CURRENT_BINARY_DIR}/gcc/include")

# GCC 14 loses the relation between bucket_count() and the mandatory rehash.
# Both valid paths assign this value before use; initialization makes that
# intent explicit without suppressing uninitialized-use diagnostics.
file(READ "${LIBCXX_SOURCE_DIR}/include/__hash_table" hash_table_source)
string(REPLACE "size_t __chash;" "" hash_without_initializer "${hash_table_source}")
string(LENGTH "${hash_table_source}" hash_source_length)
string(LENGTH "${hash_without_initializer}" hash_without_initializer_length)
string(LENGTH "size_t __chash;" hash_initializer_length)
math(EXPR hash_removed_length "${hash_source_length} - ${hash_without_initializer_length}")
if(NOT hash_removed_length EQUAL hash_initializer_length)
  message(FATAL_ERROR "The pinned libc++ hash bucket temporary has changed")
endif()
string(REPLACE
  "size_t __chash;"
  "size_t __chash = 0;"
  initialized_hash_table "${hash_table_source}"
)
file(CONFIGURE OUTPUT "${gcc_include_dir}/__hash_table"
  CONTENT "${initialized_hash_table}" @ONLY
)

# A valid push_front has front spare space, shifts existing elements right,
# or reallocates with at least one front slot. GCC 14 nevertheless diagnoses
# begin-1 against a previous one-slot allocation after inlining deque growth.
# Restrict the diagnostic exception to these two upstream overloads; retain
# all application warnings and the original implementation and optimization.
file(READ "${LIBCXX_SOURCE_DIR}/include/__split_buffer" split_buffer_source)
set(push_front_start
  "template <class _Tp, class _Allocator>\n_LIBCPP_CONSTEXPR_SINCE_CXX20 void __split_buffer<_Tp, _Allocator>::push_front(const_reference __x)"
)
set(push_back_start
  "template <class _Tp, class _Allocator>\n_LIBCPP_CONSTEXPR_SINCE_CXX20 inline _LIBCPP_HIDE_FROM_ABI void\n__split_buffer<_Tp, _Allocator>::push_back(const_reference __x)"
)
string(FIND "${split_buffer_source}" "${push_front_start}" push_front_position)
string(FIND "${split_buffer_source}" "${push_back_start}" push_back_position)
if(push_front_position LESS 0 OR push_back_position LESS_EQUAL push_front_position)
  message(FATAL_ERROR "The pinned libc++ split-buffer overloads have changed")
endif()
string(REPLACE
  "${push_front_start}"
  "#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Warray-bounds\"\n${push_front_start}"
  guarded_split_buffer "${split_buffer_source}"
)
string(REPLACE
  "${push_back_start}"
  "#pragma GCC diagnostic pop\n\n${push_back_start}"
  guarded_split_buffer "${guarded_split_buffer}"
)
file(CONFIGURE OUTPUT "${gcc_include_dir}/__split_buffer"
  CONTENT "${guarded_split_buffer}" @ONLY
)

# Scope an inlining diagnostic to one exact upstream function definition.
# The closing marker uses the function's indentation, excluding nested blocks.
function(libcxx_gcc_guard_function source_variable signature closing_marker diagnostic)
  string(FIND "${${source_variable}}" "${signature}" function_start)
  if(function_start LESS 0)
    message(FATAL_ERROR "The pinned libc++ function has changed: ${signature}")
  endif()
  string(SUBSTRING "${${source_variable}}" ${function_start} -1 function_tail)
  string(FIND "${function_tail}" "${closing_marker}" function_end)
  if(function_end LESS 0)
    message(FATAL_ERROR "Cannot locate the pinned libc++ function's closing brace")
  endif()
  string(LENGTH "${closing_marker}" closing_length)
  math(EXPR function_length "${function_end} + ${closing_length}")
  string(SUBSTRING "${function_tail}" 0 ${function_length} function_source)
  string(REPLACE
    "${function_source}"
    "#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"${diagnostic}\"\n${function_source}\n#pragma GCC diagnostic pop"
    guarded_function_source "${${source_variable}}"
  )
  set(${source_variable} "${guarded_function_source}" PARENT_SCOPE)
endfunction()

# deque establishes front spare capacity before decrementing begin(). With
# exceptions enabled at -O2, GCC 14 can also lose that invariant in these two
# callers, separately from the split-buffer map growth handled above.
file(READ "${LIBCXX_SOURCE_DIR}/include/deque" deque_source)
foreach(reference_kind IN ITEMS "const value_type&" "value_type&&")
  set(deque_signature
    "template <class _Tp, class _Allocator>\nvoid deque<_Tp, _Allocator>::push_front(${reference_kind} __v) {"
  )
  libcxx_gcc_guard_function(deque_source "${deque_signature}" "\n}" "-Warray-bounds")
endforeach()
file(CONFIGURE OUTPUT "${gcc_include_dir}/deque" CONTENT "${deque_source}" @ONLY)

# A short string initializes its short representation and never reads the
# inactive long pointer. GCC 14 can lose the __is_long() guard when inlining
# initializer-list cleanup. Suppress only this destructor's false positive.
# Start from the portable copy so its iterator-difference capacity bound stays.
file(READ "${portable_headers_dir}/string" gcc_string_source)
libcxx_gcc_guard_function(gcc_string_source
  "  inline _LIBCPP_CONSTEXPR_SINCE_CXX20 ~basic_string() {"
  "\n  }" "-Wmaybe-uninitialized"
)
file(CONFIGURE OUTPUT "${gcc_include_dir}/string" CONTENT "${gcc_string_source}" @ONLY)

# Generated copies retain the upstream license and leave the SDK untouched.
target_include_directories(zephyr_interface SYSTEM BEFORE INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:${gcc_include_dir}>"
)
