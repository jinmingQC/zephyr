/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Kernel Address Sanitizer reporting interface.
 */

#ifndef ZEPHYR_INCLUDE_DEBUG_KASAN_H_
#define ZEPHYR_INCLUDE_DEBUG_KASAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Type of memory access which caused a KASAN violation. */
enum kasan_access_type {
	KASAN_ACCESS_READ,
	KASAN_ACCESS_WRITE,
};

/** KASAN shadow provider which detected a violation. */
enum kasan_memory_type {
	KASAN_MEMORY_UNKNOWN,
	KASAN_MEMORY_HEAP,
	KASAN_MEMORY_GLOBAL,
};

/** Information describing one invalid memory access. */
struct kasan_fault {
	/** Address at which the instrumented access started. */
	uintptr_t address;
	/** First poisoned address covered by the access. */
	uintptr_t first_bad_address;
	/** Return address of the compiler-generated KASAN callback. */
	uintptr_t pc;
	/** Start of the containing heap or global object. */
	uintptr_t region_start;
	/** End of the containing heap or global object redzone. */
	uintptr_t region_end;
	/** Number of bytes accessed. */
	size_t size;
	/** Accessible object size, or zero when it is not available. */
	size_t object_size;
	/** Access direction. */
	enum kasan_access_type access;
	/** Memory provider which detected the violation. */
	enum kasan_memory_type memory;
	/** Compiler-provided global object name, when available. */
	const char *object_name;
	/** Compiler-provided source file containing the object, when available. */
	const char *source_file;
	/** Compiler-provided source line, when available. */
	uint32_t source_line;
	/** Compiler-provided source column, when available. */
	uint32_t source_column;
};

/**
 * @brief Handle a KASAN violation.
 *
 * The default weak implementation calls heap_kasan_report() for heap faults
 * to preserve the existing Heap KASAN test hook, and otherwise calls
 * k_panic(). Applications may override this function to integrate KASAN with
 * a platform-specific crash collector. The diagnostic is printed before this
 * callback is invoked.
 *
 * @param fault Description of the invalid access.
 */
void kasan_report(const struct kasan_fault *fault);

/** @cond INTERNAL_HIDDEN */

/* Common runtime entry point used by compiler ABI callbacks and interceptors. */
void z_kasan_check_access(uintptr_t address, size_t size, enum kasan_access_type access,
			  uintptr_t pc);

#if defined(CONFIG_SYS_HEAP_KASAN)
bool z_heap_kasan_find_fault(uintptr_t address, size_t size, struct kasan_fault *fault);
#endif

#if defined(CONFIG_KASAN_GLOBAL)
bool z_kasan_global_find_fault(uintptr_t address, size_t size, struct kasan_fault *fault);
#endif

/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DEBUG_KASAN_H_ */
