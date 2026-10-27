# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Included after libcxx_source_runtime has been configured.
set(LIBUNWIND_SOURCE_DIR "" CACHE PATH "Path to unpacked libunwind 19.1.7 sources")

if(NOT LIBUNWIND_SOURCE_DIR)
  FetchContent_Declare(libunwind_source
    URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/libunwind-19.1.7.src.tar.xz"
    URL_HASH "SHA256=10e650f436bc416674f01b5e01177405886f4d0f0b2568c6808044eacad52ea0"
  )
  FetchContent_GetProperties(libunwind_source)
  if(NOT libunwind_source_POPULATED)
    FetchContent_Populate(libunwind_source)
  endif()
  set(LIBUNWIND_SOURCE_DIR "${libunwind_source_SOURCE_DIR}")
endif()

# libc++abi must use the unwinder headers matching the runtime in this image.
target_include_directories(libcxx_source_runtime SYSTEM BEFORE PRIVATE
  "${LIBUNWIND_SOURCE_DIR}/include"
)

# ARM TARGET2 type references use bare-metal relocations, without a GOT indirection.
target_compile_definitions(libcxx_source_runtime PRIVATE LIBCXXABI_BAREMETAL)

# Preserve upstream implementation and licenses in the build tree. Only the
# private cache lock is replaced; no pthread implementation is required.
set(unwind_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/unwind")
file(MAKE_DIRECTORY "${unwind_generated_dir}")
configure_file("${LIBUNWIND_SOURCE_DIR}/src/libunwind.cpp"
  "${unwind_generated_dir}/libunwind.cpp" COPYONLY
)
file(READ "${LIBUNWIND_SOURCE_DIR}/src/UnwindCursor.hpp" unwind_cursor)
string(REPLACE
  "#include \"RWMutex.hpp\""
  "#include \"zephyr_unwind_mutex.hpp\""
  native_unwind_cursor "${unwind_cursor}"
)
if(native_unwind_cursor STREQUAL unwind_cursor)
  message(FATAL_ERROR "The pinned libunwind cache lock include has changed")
endif()
file(CONFIGURE OUTPUT "${unwind_generated_dir}/UnwindCursor.hpp"
  CONTENT "${native_unwind_cursor}" @ONLY
)

zephyr_library_named(libcxx_unwind)
zephyr_library_include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${LIBUNWIND_SOURCE_DIR}/include"
  "${LIBUNWIND_SOURCE_DIR}/src"
)
zephyr_library_sources(
  "${unwind_generated_dir}/libunwind.cpp"
  "${LIBUNWIND_SOURCE_DIR}/src/Unwind-EHABI.cpp"
  "${LIBUNWIND_SOURCE_DIR}/src/UnwindLevel1.c"
  "${LIBUNWIND_SOURCE_DIR}/src/UnwindLevel1-gcc-ext.c"
  "${LIBUNWIND_SOURCE_DIR}/src/UnwindRegistersRestore.S"
  "${LIBUNWIND_SOURCE_DIR}/src/UnwindRegistersSave.S"
)
zephyr_library_compile_definitions(
  _LIBUNWIND_IS_BAREMETAL
  _LIBUNWIND_IS_NATIVE_ONLY
  _LIBUNWIND_SUPPORT_FRAME_APIS
  _LIBUNWIND_NO_HEAP
  NDEBUG
)
# Static images use the linker-provided unwind tables. Avoid heap allocation
# while unwinding, including during bad_alloc handling. Dynamic/JIT frames
# are not supported by this profile.
set_property(TARGET libcxx_unwind PROPERTY CXX_SCAN_FOR_MODULES OFF)
set_source_files_properties(
  "${unwind_generated_dir}/libunwind.cpp"
  "${LIBUNWIND_SOURCE_DIR}/src/Unwind-EHABI.cpp"
  PROPERTIES COMPILE_OPTIONS "-std=c++17;-fno-exceptions;-fno-rtti"
)
# The unwinder must unwind its own C and C++ frames, even with exceptions disabled.
zephyr_library_compile_options(
  $<$<COMPILE_LANGUAGE:C,CXX>:-funwind-tables>
  $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-shadow>
  $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-double-promotion>
)
if(NOT CONFIG_ARM)
  # Some bare-metal linkers omit the optional DWARF index. Without it,
  # libunwind scans the retained .eh_frame using Zephyr's section bounds.
  execute_process(COMMAND "${CMAKE_LINKER}" --help
    OUTPUT_VARIABLE unwind_linker_help
    ERROR_QUIET
    RESULT_VARIABLE unwind_linker_help_result
  )
  if(unwind_linker_help_result EQUAL 0 AND unwind_linker_help MATCHES "--eh-frame-hdr")
    zephyr_link_libraries("-Wl,--eh-frame-hdr")
  endif()
endif()
