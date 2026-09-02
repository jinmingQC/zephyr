/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/debug/kasan.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SYS_HEAP_KASAN)
#include <zephyr/sys/heap_kasan.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define KASAN_CALLER() \
	((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))

ATOMIC_DEFINE(kasan_reporting, CONFIG_MP_MAX_NUM_CPUS);

static unsigned int kasan_cpu_id(void)
{
#if defined(CONFIG_SMP)
	return arch_curr_cpu()->id;
#else
	return 0U;
#endif
}

static void kasan_print_fault(const struct kasan_fault *fault)
{
	const char *access = fault->access == KASAN_ACCESS_WRITE ? "write" : "read";
	const char *memory = "unknown";

	if (fault->memory == KASAN_MEMORY_HEAP) {
		memory = "heap";
	} else if (fault->memory == KASAN_MEMORY_GLOBAL) {
		memory = "global";
	}

	printk("KASAN: invalid %s of size %zu at %p (first bad %p, caller %p)\n",
	       access, fault->size, (void *)fault->address,
	       (void *)fault->first_bad_address, (void *)fault->pc);
	printk("KASAN: %s region [%p, %p)", memory,
	       (void *)fault->region_start, (void *)fault->region_end);
	if (fault->object_size != 0U) {
		printk(", object size %zu", fault->object_size);
	}
	printk("\n");

	if (fault->object_name != NULL) {
		printk("KASAN: object '%s'", fault->object_name);
		if (fault->source_file != NULL) {
			printk(" declared at %s:%u:%u", fault->source_file,
			       fault->source_line, fault->source_column);
		}
		printk("\n");
	}
}

void __weak kasan_report(const struct kasan_fault *fault)
{
#if defined(CONFIG_SYS_HEAP_KASAN)
	if (fault->memory == KASAN_MEMORY_HEAP) {
		heap_kasan_report(fault->first_bad_address, fault->size);
		return;
	}
#endif

	k_panic();
}

static void kasan_handle_fault(struct kasan_fault *fault)
{
	unsigned int cpu = kasan_cpu_id();

	if (atomic_test_and_set_bit(kasan_reporting, cpu)) {
		return;
	}

	kasan_print_fault(fault);

	/*
	 * Clear before entering an overridable callback. This keeps diagnostic
	 * formatting recursion-safe while preserving Heap KASAN's documented
	 * ability to longjmp() from a test override.
	 */
	atomic_clear_bit(kasan_reporting, cpu);
	kasan_report(fault);
}

void z_kasan_check_access(uintptr_t address, size_t size, enum kasan_access_type access,
			  uintptr_t pc)
{
	struct kasan_fault fault = {
		.address = address,
		.first_bad_address = address,
		.pc = pc,
		.size = size,
		.access = access,
		.memory = KASAN_MEMORY_UNKNOWN,
	};

	if (size == 0U) {
		return;
	}

#if defined(CONFIG_SYS_HEAP_KASAN)
	if (z_heap_kasan_find_fault(address, size, &fault)) {
		kasan_handle_fault(&fault);
		return;
	}
#endif

#if defined(CONFIG_KASAN_GLOBAL)
	if (z_kasan_global_find_fault(address, size, &fault)) {
		kasan_handle_fault(&fault);
	}
#endif
}

#define DEFINE_ASAN_ACCESS_CALLBACKS(width)                                             \
	void __asan_load##width##_noabort(uintptr_t address)                              \
	{                                                                                  \
		if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {                             \
			z_kasan_check_access(address, width, KASAN_ACCESS_READ, KASAN_CALLER()); \
		}                                                                          \
	}                                                                                  \
	void __asan_store##width##_noabort(uintptr_t address)                             \
	{                                                                                  \
		z_kasan_check_access(address, width, KASAN_ACCESS_WRITE, KASAN_CALLER());    \
	}

DEFINE_ASAN_ACCESS_CALLBACKS(1)
DEFINE_ASAN_ACCESS_CALLBACKS(2)
DEFINE_ASAN_ACCESS_CALLBACKS(4)
DEFINE_ASAN_ACCESS_CALLBACKS(8)
DEFINE_ASAN_ACCESS_CALLBACKS(16)

void __asan_loadN_noabort(uintptr_t address, size_t size)
{
	if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
		z_kasan_check_access(address, size, KASAN_ACCESS_READ, KASAN_CALLER());
	}
}

void __asan_storeN_noabort(uintptr_t address, size_t size)
{
	z_kasan_check_access(address, size, KASAN_ACCESS_WRITE, KASAN_CALLER());
}

void __asan_init(void)
{
	/* Required compiler ABI symbol. */
}

void __asan_version_mismatch_check_v6(void)
{
	/* Required by older GCC releases. */
}

void __asan_version_mismatch_check_v7(void)
{
	/* Required by older GCC releases. */
}

void __asan_version_mismatch_check_v8(void)
{
	/* Required by current GCC releases. */
}

void __asan_handle_no_return(void)
{
	/* Stack poisoning is not implemented. */
}

void __asan_alloca_poison(uintptr_t address, size_t size)
{
	ARG_UNUSED(address);
	ARG_UNUSED(size);
}

void __asan_allocas_unpoison(uintptr_t top, uintptr_t bottom)
{
	ARG_UNUSED(top);
	ARG_UNUSED(bottom);
}

static size_t kasan_checked_strlen(const char *str, uintptr_t pc)
{
	size_t len = 0U;

	for (;;) {
		if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
			z_kasan_check_access((uintptr_t)&str[len], 1U, KASAN_ACCESS_READ, pc);
		}
		if (str[len] == '\0') {
			return len;
		}
		len++;
	}
}

static size_t kasan_checked_strnlen(const char *str, size_t maxlen, uintptr_t pc)
{
	size_t len;

	for (len = 0U; len < maxlen; len++) {
		if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
			z_kasan_check_access((uintptr_t)&str[len], 1U, KASAN_ACCESS_READ, pc);
		}
		if (str[len] == '\0') {
			break;
		}
	}

	return len;
}

void *__asan_memset(void *dst, int value, size_t size)
{
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, KASAN_CALLER());
	return __builtin_memset(dst, value, size);
}

void *__asan_memcpy(void *dst, const void *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
		z_kasan_check_access((uintptr_t)src, size, KASAN_ACCESS_READ, pc);
	}
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return __builtin_memcpy(dst, src, size);
}

void *__asan_memmove(void *dst, const void *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
		z_kasan_check_access((uintptr_t)src, size, KASAN_ACCESS_READ, pc);
	}
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return __builtin_memmove(dst, src, size);
}

char *__asan_strcpy(char *dst, const char *src)
{
	uintptr_t pc = KASAN_CALLER();
	size_t size = kasan_checked_strlen(src, pc) + 1U;

	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return strcpy(dst, src);
}

char *__asan_strncpy(char *dst, const char *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	(void)kasan_checked_strnlen(src, size, pc);
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return strncpy(dst, src, size);
}

char *__asan_strcat(char *dst, const char *src)
{
	uintptr_t pc = KASAN_CALLER();
	size_t dst_len = kasan_checked_strlen(dst, pc);
	size_t src_len = kasan_checked_strlen(src, pc);

	z_kasan_check_access((uintptr_t)&dst[dst_len], src_len + 1U,
			     KASAN_ACCESS_WRITE, pc);
	return strcat(dst, src);
}

char *__asan_strncat(char *dst, const char *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();
	size_t dst_len = kasan_checked_strlen(dst, pc);
	size_t src_len = kasan_checked_strnlen(src, size, pc);

	z_kasan_check_access((uintptr_t)&dst[dst_len], src_len + 1U,
			     KASAN_ACCESS_WRITE, pc);
	return strncat(dst, src, size);
}

int __asan_vsnprintf(char *dst, size_t size, const char *format, va_list ap)
{
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, KASAN_CALLER());
	return vsnprintf(dst, size, format, ap);
}

int __asan_snprintf(char *dst, size_t size, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = __asan_vsnprintf(dst, size, format, ap);
	va_end(ap);
	return ret;
}

int __asan_vsprintf(char *dst, const char *format, va_list ap)
{
	va_list copy;
	int needed;

	va_copy(copy, ap);
	needed = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (needed >= 0) {
		z_kasan_check_access((uintptr_t)dst, (size_t)needed + 1U,
				     KASAN_ACCESS_WRITE, KASAN_CALLER());
	}
	return vsprintf(dst, format, ap);
}

int __asan_sprintf(char *dst, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = __asan_vsprintf(dst, format, ap);
	va_end(ap);
	return ret;
}

#if defined(CONFIG_SYS_HEAP_KASAN_EXTENSIONS)
char *__asan_stpcpy(char *dst, const char *src)
{
	uintptr_t pc = KASAN_CALLER();
	size_t size = kasan_checked_strlen(src, pc) + 1U;

	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return stpcpy(dst, src);
}

char *__asan_stpncpy(char *dst, const char *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	(void)kasan_checked_strnlen(src, size, pc);
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return stpncpy(dst, src, size);
}

size_t __asan_strlcpy(char *dst, const char *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	(void)kasan_checked_strlen(src, pc);
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return strlcpy(dst, src, size);
}

size_t __asan_strlcat(char *dst, const char *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	(void)kasan_checked_strnlen(dst, size, pc);
	(void)kasan_checked_strlen(src, pc);
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return strlcat(dst, src, size);
}

void *__asan_memccpy(void *dst, const void *src, int value, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
		z_kasan_check_access((uintptr_t)src, size, KASAN_ACCESS_READ, pc);
	}
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	return memccpy(dst, src, value, size);
}

void *__asan_mempcpy(void *dst, const void *src, size_t size)
{
	uintptr_t pc = KASAN_CALLER();

	if (IS_ENABLED(CONFIG_KASAN_INSTRUMENT_READS)) {
		z_kasan_check_access((uintptr_t)src, size, KASAN_ACCESS_READ, pc);
	}
	z_kasan_check_access((uintptr_t)dst, size, KASAN_ACCESS_WRITE, pc);
	__builtin_memcpy(dst, src, size);
	return (uint8_t *)dst + size;
}

char *__asan_fgets(char *buffer, int size, FILE *stream)
{
	if (size <= 0) {
		return NULL;
	}
	z_kasan_check_access((uintptr_t)buffer, (size_t)size,
			     KASAN_ACCESS_WRITE, KASAN_CALLER());
	return fgets(buffer, size, stream);
}
#endif /* CONFIG_SYS_HEAP_KASAN_EXTENSIONS */
