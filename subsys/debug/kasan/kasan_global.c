/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/debug/kasan.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* GCC and Clang __asan_global descriptor ABI. Scalar fields are pointer-sized. */
struct kasan_global_descriptor {
	uintptr_t begin;
	uintptr_t size;
	uintptr_t size_with_redzone;
	const char *name;
	const char *module_name;
	uintptr_t has_dynamic_init;
	const struct kasan_global_source_location *location;
	uintptr_t odr_indicator;
};

struct kasan_global_source_location {
	const char *filename;
	uint32_t line;
	uint32_t column;
};

static atomic_ptr_t global_index[CONFIG_KASAN_GLOBAL_MAX_OBJECTS];
static atomic_t global_count;
static struct k_spinlock global_lock;
static atomic_t global_generation;

/*
 * Registration changes are rare, while lookups run on every instrumented
 * access. Atomic slots plus the generation counter keep the read side
 * lock-free without introducing C data races during a table update.
 */

static const struct kasan_global_descriptor *global_index_get(size_t index)
{
	return (const struct kasan_global_descriptor *)atomic_ptr_get(&global_index[index]);
}

static bool descriptor_valid(const struct kasan_global_descriptor *descriptor)
{
	return descriptor->size != 0U &&
	       descriptor->size <= descriptor->size_with_redzone &&
	       descriptor->begin <= UINTPTR_MAX - descriptor->size_with_redzone;
}

static size_t global_lower_bound(uintptr_t address, size_t count)
{
	size_t first = 0U;

	while (first < count) {
		size_t middle = first + (count - first) / 2U;

		if (global_index_get(middle)->begin < address) {
			first = middle + 1U;
		} else {
			count = middle;
		}
	}

	return first;
}

static bool register_descriptor(const struct kasan_global_descriptor *descriptor)
{
	size_t count = (size_t)atomic_get(&global_count);
	size_t position;

	if (!descriptor_valid(descriptor)) {
		return true;
	}

	position = global_lower_bound(descriptor->begin, count);
	for (size_t i = position;
	     i < count && global_index_get(i)->begin == descriptor->begin; i++) {
		if (global_index_get(i) == descriptor) {
			return true;
		}
	}

	if (count == ARRAY_SIZE(global_index)) {
		return false;
	}

	for (size_t i = count; i > position; i--) {
		atomic_ptr_set(&global_index[i],
			       (atomic_ptr_val_t)global_index_get(i - 1U));
	}
	atomic_ptr_set(&global_index[position], (atomic_ptr_val_t)descriptor);
	atomic_set(&global_count, (atomic_val_t)(count + 1U));
	return true;
}

void __asan_register_globals(struct kasan_global_descriptor *descriptors, size_t count)
{
	k_spinlock_key_t key = k_spin_lock(&global_lock);
	bool success = true;

	atomic_inc(&global_generation);
	for (size_t i = 0U; i < count; i++) {
		if (!register_descriptor(&descriptors[i])) {
			success = false;
			break;
		}
	}
	atomic_inc(&global_generation);
	k_spin_unlock(&global_lock, key);

	if (!success) {
		printk("KASAN: global descriptor index is full; increase "
		       "CONFIG_KASAN_GLOBAL_MAX_OBJECTS\n");
		k_panic();
	}
}

void __asan_unregister_globals(struct kasan_global_descriptor *descriptors, size_t count)
{
	k_spinlock_key_t key = k_spin_lock(&global_lock);
	size_t registered = (size_t)atomic_get(&global_count);

	atomic_inc(&global_generation);
	for (size_t d = 0U; d < count; d++) {
		for (size_t i = 0U; i < registered; i++) {
			if (global_index_get(i) != &descriptors[d]) {
				continue;
			}
			for (; i + 1U < registered; i++) {
				atomic_ptr_set(&global_index[i],
					       (atomic_ptr_val_t)global_index_get(i + 1U));
			}
			registered--;
			break;
		}
	}
	atomic_set(&global_count, (atomic_val_t)registered);
	atomic_inc(&global_generation);
	k_spin_unlock(&global_lock, key);
}

void __asan_before_dynamic_init(const char *module_name)
{
	ARG_UNUSED(module_name);
}

void __asan_after_dynamic_init(void)
{
}

static bool descriptor_find_fault(const struct kasan_global_descriptor *descriptor,
				  uintptr_t access_start, uintptr_t access_end,
				  struct kasan_fault *fault)
{
	uintptr_t poison_start = descriptor->begin + descriptor->size;
	uintptr_t poison_end = descriptor->begin + descriptor->size_with_redzone;

	if (access_start >= poison_end || access_end <= poison_start) {
		return false;
	}

	fault->first_bad_address = MAX(access_start, poison_start);
	fault->region_start = descriptor->begin;
	fault->region_end = poison_end;
	fault->object_size = descriptor->size;
	fault->memory = KASAN_MEMORY_GLOBAL;
	fault->object_name = descriptor->name;
	if (descriptor->location != NULL) {
		fault->source_file = descriptor->location->filename;
		fault->source_line = descriptor->location->line;
		fault->source_column = descriptor->location->column;
	}
	return true;
}

static bool global_find_fault(uintptr_t address, size_t size, size_t count,
			      struct kasan_fault *fault)
{
	uintptr_t access_end = size > UINTPTR_MAX - address ? UINTPTR_MAX : address + size;
	size_t position = global_lower_bound(address, count);

	if (position > 0U) {
		position--;
	}

	for (size_t i = position; i < count; i++) {
		const struct kasan_global_descriptor *descriptor = global_index_get(i);

		if (descriptor->begin >= access_end) {
			break;
		}
		if (descriptor_find_fault(descriptor, address, access_end, fault)) {
			return true;
		}
	}

	return false;
}

bool z_kasan_global_find_fault(uintptr_t address, size_t size, struct kasan_fault *fault)
{
	for (;;) {
		atomic_val_t generation = atomic_get(&global_generation);
		bool found;

		if ((generation & 1) != 0) {
			continue;
		}
		found = global_find_fault(address, size,
					  (size_t)atomic_get(&global_count), fault);
		if (generation == atomic_get(&global_generation)) {
			return found;
		}
	}
}
