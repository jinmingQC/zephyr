# SPDX-License-Identifier: Apache-2.0

# Zephyr's generated app-memory scripts quote archive globs. LLD treats those
# quoted globs literally, unlike GNU ld. Correct only the generated build files,
# after their normal generation and before any linker-script preprocessing.
function(cpp_runtime_fix_lld_app_partitions)
  set(fix_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/fix_lld_app_partitions.py")
  foreach(script IN ITEMS "${APP_SMEM_UNALIGNED_LD}" "${APP_SMEM_ALIGNED_LD}")
    if(NOT script MATCHES "[.]ld$")
      message(FATAL_ERROR "libc++ userspace LLD requires the Zephyr .ld partition generator")
    endif()
    add_custom_command(OUTPUT "${script}" APPEND
      COMMAND "${PYTHON_EXECUTABLE}" "${fix_script}" "${script}"
      DEPENDS "${fix_script}"
    )
  endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${ZEPHYR_BASE}" CALL cpp_runtime_fix_lld_app_partitions)
