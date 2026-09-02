/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Lightweight heap KASAN shadow provider for sys_heap.
 *
 * Shadow map: one bit per SYS_HEAP_KASAN_GRANULE bytes. A set bit means
 * poisoned. Compiler ABI callbacks and libc interceptors live in the common
 * KASAN runtime under subsys/debug/kasan.
 */

#include <zephyr/debug/kasan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/sys/heap_kasan.h>
#include <zephyr/sys/sys_heap.h>

#include "heap.h"

LOG_MODULE_DECLARE(os_heap, CONFIG_SYS_HEAP_LOG_LEVEL);

#define ASAN_GRANULE         SYS_HEAP_KASAN_GRANULE
#define ASAN_SLOTS_PER_CHUNK (CHUNK_UNIT / ASAN_GRANULE)

BUILD_ASSERT(IS_POWER_OF_TWO(SYS_HEAP_KASAN_GRANULE) &&
	     SYS_HEAP_KASAN_GRANULE <= CHUNK_UNIT,
	     "SYS_HEAP_KASAN_GRANULE must be a power of two <= CHUNK_UNIT");

struct heap_kasan_region {
	uintptr_t base;
	uintptr_t span;
	uint32_t *bundles;
	struct sys_heap *heap;
	sys_bitarray_t *bitarray;
};

static struct heap_kasan_region regions[CONFIG_SYS_HEAP_KASAN_MAX_HEAPS];
static uintptr_t heap_min = UINTPTR_MAX;
static uintptr_t heap_max;
static int region_count;

#if defined(CONFIG_THREAD_LOCAL_STORAGE)
static Z_THREAD_LOCAL int last_region_index;
#endif

static void heap_kasan_init_heap(struct sys_heap *heap);

void heap_kasan_register(struct sys_heap *heap, sys_bitarray_t *bitarray)
{
	if (region_count >= ARRAY_SIZE(regions)) {
		LOG_ERR("registration table full; increase CONFIG_SYS_HEAP_KASAN_MAX_HEAPS");
		return;
	}

	regions[region_count].heap = heap;
	regions[region_count].bitarray = bitarray;
	region_count++;
}

void heap_sanitizer_on_init(struct sys_heap *heap, void *mem, size_t bytes)
{
	ARG_UNUSED(mem);
	ARG_UNUSED(bytes);

	for (int i = 0; i < region_count; i++) {
		if (regions[i].heap != heap) {
			continue;
		}

		heap->kasan_ba = regions[i].bitarray;
		heap_kasan_init_heap(heap);
		regions[i].base = (uintptr_t)chunk_buf(heap->heap);
		regions[i].span = (uintptr_t)heap->heap->end_chunk * CHUNK_UNIT;
		regions[i].bundles = heap->kasan_ba->bundles;
		return;
	}
}

static bool region_contains(int index, uintptr_t address)
{
	return regions[index].base != 0U && address >= regions[index].base &&
	       address - regions[index].base < regions[index].span;
}

static const struct heap_kasan_region *find_region(uintptr_t address)
{
	if (address < heap_min || address >= heap_max) {
		return NULL;
	}

#if defined(CONFIG_THREAD_LOCAL_STORAGE)
	if (region_contains(last_region_index, address)) {
		return &regions[last_region_index];
	}
#endif

	for (int i = 0; i < region_count; i++) {
		if (!region_contains(i, address)) {
			continue;
		}
#if defined(CONFIG_THREAD_LOCAL_STORAGE)
		last_region_index = i;
#endif
		return &regions[i];
	}

	return NULL;
}

static void heap_kasan_init_heap(struct sys_heap *heap)
{
	struct z_heap *internal = heap->heap;
	size_t slots = internal->end_chunk * ASAN_SLOTS_PER_CHUNK;
	uintptr_t base = (uintptr_t)chunk_buf(internal);
	uintptr_t end = base + (uintptr_t)internal->end_chunk * CHUNK_UNIT;

	__ASSERT(slots <= heap->kasan_ba->num_bundles * 32U,
		 "Heap KASAN shadow is undersized");

	heap->kasan_ba->num_bits = slots;
	__builtin_memset(heap->kasan_ba->bundles, 0xff,
			 heap->kasan_ba->num_bundles * sizeof(uint32_t));

	heap_min = MIN(heap_min, base);
	heap_max = MAX(heap_max, end);
}

void heap_sanitizer_on_alloc(struct sys_heap *heap, void *mem, size_t bytes)
{
	if (heap->kasan_ba == NULL || bytes == 0U) {
		return;
	}

	uintptr_t base = (uintptr_t)chunk_buf(heap->heap);
	size_t first_slot = ((uintptr_t)mem - base) / ASAN_GRANULE;
	size_t last_slot = ((uintptr_t)mem + bytes - 1U - base) / ASAN_GRANULE;

	sys_bitarray_clear_region(heap->kasan_ba, last_slot - first_slot + 1U, first_slot);
}

void heap_sanitizer_on_free(struct sys_heap *heap, void *mem, size_t bytes)
{
	if (heap->kasan_ba == NULL || bytes == 0U) {
		return;
	}

	uintptr_t base = (uintptr_t)chunk_buf(heap->heap);
	size_t first_slot = ((uintptr_t)mem - base) / ASAN_GRANULE;
	size_t last_slot = ((uintptr_t)mem + bytes - 1U - base) / ASAN_GRANULE;

	sys_bitarray_set_region(heap->kasan_ba, last_slot - first_slot + 1U, first_slot);
}

void __weak heap_kasan_report(uintptr_t address, size_t size)
{
	ARG_UNUSED(address);
	ARG_UNUSED(size);
	k_panic();
}

/*
 * No lock by design: allocation and access to the same bytes without external
 * synchronization is already a race. A torn shadow read can only affect that
 * racing access, while locking every compiler callback would be prohibitive.
 */
bool z_heap_kasan_find_fault(uintptr_t address, size_t size, struct kasan_fault *fault)
{
	const struct heap_kasan_region *region = find_region(address);
	uintptr_t offset;
	size_t first_slot;
	size_t last_slot;
	size_t first_bundle;
	size_t last_bundle;

	if (region == NULL || size == 0U) {
		return false;
	}

	offset = address - region->base;
	if (size > region->span - offset) {
		fault->first_bad_address = region->base + region->span;
		fault->region_start = region->base;
		fault->region_end = region->base + region->span;
		fault->memory = KASAN_MEMORY_HEAP;
		return true;
	}

	first_slot = offset / ASAN_GRANULE;
	last_slot = (offset + size - 1U) / ASAN_GRANULE;
	first_bundle = first_slot / 32U;
	last_bundle = last_slot / 32U;

	for (size_t bundle = first_bundle; bundle <= last_bundle; bundle++) {
		uint32_t shadow = region->bundles[bundle];

		if (bundle == first_bundle) {
			shadow &= UINT32_MAX << (first_slot & 31U);
		}
		if (bundle == last_bundle) {
			shadow &= UINT32_MAX >> (31U - (last_slot & 31U));
		}
		if (shadow == 0U) {
			continue;
		}

		size_t bit = (size_t)__builtin_ctz(shadow);
		uintptr_t poisoned = region->base + ((bundle * 32U + bit) * ASAN_GRANULE);

		fault->first_bad_address = MAX(address, poisoned);
		fault->region_start = region->base;
		fault->region_end = region->base + region->span;
		fault->memory = KASAN_MEMORY_HEAP;
		return true;
	}

	return false;
}
