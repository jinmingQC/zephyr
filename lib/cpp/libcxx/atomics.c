/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

/*
 * The compiler emits sized 1/2/4-byte entry points on targets without native
 * atomics. This profile supplies only the missing 64-bit and generic ABI.
 */
BUILD_ASSERT(__atomic_always_lock_free(1, 0) && __atomic_always_lock_free(2, 0) &&
		     __atomic_always_lock_free(4, 0),
	     "libc++ requires native lock-free 1-, 2-, and 4-byte atomics");

/*
 * The native spinlock serializes fallback accesses across CPUs and prevents a
 * local interrupt from deadlocking on a preempted operation. Full fences also
 * order these accesses with lock-free atomics. All orders use seq_cst semantics.
 */
static struct k_spinlock atomic_lock;

static k_spinlock_key_t atomic_lock_acquire(void)
{
	k_spinlock_key_t key = k_spin_lock(&atomic_lock);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return key;
}

static void atomic_lock_release(k_spinlock_key_t key)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	k_spin_unlock(&atomic_lock, key);
}

/* Alternate C identifiers avoid redefining compiler intrinsic declarations. */
uint64_t z_libcxx_atomic_load_8(const volatile void *object, int order) __asm__("__atomic_load_8");
void z_libcxx_atomic_store_8(volatile void *object, uint64_t desired,
			     int order) __asm__("__atomic_store_8");
uint64_t z_libcxx_atomic_exchange_8(volatile void *object, uint64_t desired,
				    int order) __asm__("__atomic_exchange_8");
bool z_libcxx_atomic_compare_exchange_8(volatile void *object, void *expected, uint64_t desired,
					int success,
					int failure) __asm__("__atomic_compare_exchange_8");

/*
 * Sized and generic entry points must use the same access mechanism. In
 * particular, aligned native 64-bit accesses cannot mix a private lock with
 * compiler-emitted lock-free accesses to the same object.
 */
uint64_t z_libcxx_atomic_load_8(const volatile void *object, int order)
{
	ARG_UNUSED(order);
	if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
		return __atomic_load_n((const volatile uint64_t *)object, __ATOMIC_SEQ_CST);
	}
	k_spinlock_key_t key = atomic_lock_acquire();
	uint64_t value;

	memcpy(&value, (const void *)object, sizeof(value));
	atomic_lock_release(key);
	return value;
}

void z_libcxx_atomic_store_8(volatile void *object, uint64_t desired, int order)
{
	ARG_UNUSED(order);
	if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
		__atomic_store_n((volatile uint64_t *)object, desired, __ATOMIC_SEQ_CST);
		return;
	}
	k_spinlock_key_t key = atomic_lock_acquire();

	memcpy((void *)object, &desired, sizeof(desired));
	atomic_lock_release(key);
}

uint64_t z_libcxx_atomic_exchange_8(volatile void *object, uint64_t desired, int order)
{
	ARG_UNUSED(order);
	if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
		return __atomic_exchange_n((volatile uint64_t *)object, desired, __ATOMIC_SEQ_CST);
	}
	k_spinlock_key_t key = atomic_lock_acquire();
	uint64_t previous;

	memcpy(&previous, (const void *)object, sizeof(previous));
	memcpy((void *)object, &desired, sizeof(desired));
	atomic_lock_release(key);
	return previous;
}

bool z_libcxx_atomic_compare_exchange_8(volatile void *object, void *expected, uint64_t desired,
					int success, int failure)
{
	ARG_UNUSED(success);
	ARG_UNUSED(failure);
	if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
		uint64_t old;

		memcpy(&old, expected, sizeof(old));
		bool matched =
			__atomic_compare_exchange_n((volatile uint64_t *)object, &old, desired,
						    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

		if (!matched) {
			memcpy(expected, &old, sizeof(old));
		}
		return matched;
	}
	k_spinlock_key_t key = atomic_lock_acquire();
	bool matched = memcmp((const void *)object, expected, sizeof(desired)) == 0;

	if (matched) {
		memcpy((void *)object, &desired, sizeof(desired));
	} else {
		memcpy(expected, (const void *)object, sizeof(desired));
	}
	atomic_lock_release(key);
	return matched;
}

#define DEFINE_ATOMIC_RMW(name, expression)                                                        \
	uint64_t z_libcxx_atomic_fetch_##name##_8(volatile void *object, uint64_t desired,         \
						  int order) __asm__("__atomic_fetch_" #name       \
								     "_8");                        \
	uint64_t z_libcxx_atomic_##name##_fetch_8(volatile void *object, uint64_t desired,         \
						  int order) __asm__("__atomic_" #name             \
								     "_fetch_8");                  \
	uint64_t z_libcxx_atomic_fetch_##name##_8(volatile void *object, uint64_t desired,         \
						  int order)                                       \
	{                                                                                          \
		ARG_UNUSED(order);                                                                 \
		if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {            \
			return __atomic_fetch_##name((volatile uint64_t *)object, desired,         \
						     __ATOMIC_SEQ_CST);                            \
		}                                                                                  \
		k_spinlock_key_t key = atomic_lock_acquire();                                      \
		uint64_t previous;                                                                 \
                                                                                                   \
		memcpy(&previous, (const void *)object, sizeof(previous));                         \
		uint64_t value = (expression);                                                     \
                                                                                                   \
		memcpy((void *)object, &value, sizeof(value));                                     \
		atomic_lock_release(key);                                                          \
		return previous;                                                                   \
	}                                                                                          \
	uint64_t z_libcxx_atomic_##name##_fetch_8(volatile void *object, uint64_t desired,         \
						  int order)                                       \
	{                                                                                          \
		if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {            \
			return __atomic_##name##_fetch((volatile uint64_t *)object, desired,       \
						       __ATOMIC_SEQ_CST);                          \
		}                                                                                  \
		uint64_t previous = z_libcxx_atomic_fetch_##name##_8(object, desired, order);      \
                                                                                                   \
		return (expression);                                                               \
	}

DEFINE_ATOMIC_RMW(add, previous + desired)
DEFINE_ATOMIC_RMW(sub, previous - desired)
DEFINE_ATOMIC_RMW(and, (previous & desired))
DEFINE_ATOMIC_RMW(or, previous | desired)
DEFINE_ATOMIC_RMW(xor, previous ^ desired)
DEFINE_ATOMIC_RMW(nand, (~(previous & desired)))

#undef DEFINE_ATOMIC_RMW

void z_libcxx_atomic_load(size_t size, const volatile void *object, void *result,
			  int order) __asm__("__atomic_load");
void z_libcxx_atomic_store(size_t size, volatile void *object, void *desired,
			   int order) __asm__("__atomic_store");
void z_libcxx_atomic_exchange(size_t size, volatile void *object, void *desired, void *result,
			      int order) __asm__("__atomic_exchange");
bool z_libcxx_atomic_compare_exchange(size_t size, volatile void *object, void *expected,
				      void *desired, int success,
				      int failure) __asm__("__atomic_compare_exchange");
bool z_libcxx_atomic_is_lock_free(size_t size,
				  const volatile void *object) __asm__("__atomic_is_lock_free");

/*
 * Generic calls can also name native atomic objects. Dispatch aligned,
 * lock-free widths to compiler intrinsics so a generic access remains atomic
 * with respect to inline accesses that do not acquire atomic_lock.
 */
void z_libcxx_atomic_load(size_t size, const volatile void *object, void *result, int order)
{
	ARG_UNUSED(order);
	switch (size) {
	case 1:
		if (__atomic_always_lock_free(1, 0) && ((uintptr_t)object % 1U) == 0) {
			uint8_t value;

			value = __atomic_load_n((const volatile uint8_t *)object, __ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	case 2:
		if (__atomic_always_lock_free(2, 0) && ((uintptr_t)object % 2U) == 0) {
			uint16_t value;

			value = __atomic_load_n((const volatile uint16_t *)object,
						__ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	case 4:
		if (__atomic_always_lock_free(4, 0) && ((uintptr_t)object % 4U) == 0) {
			uint32_t value;

			value = __atomic_load_n((const volatile uint32_t *)object,
						__ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	case 8:
		if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
			uint64_t value;

			value = __atomic_load_n((const volatile uint64_t *)object,
						__ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	default:
		break;
	}
	k_spinlock_key_t key = atomic_lock_acquire();

	memcpy(result, (const void *)object, size);
	atomic_lock_release(key);
}

void z_libcxx_atomic_store(size_t size, volatile void *object, void *desired, int order)
{
	ARG_UNUSED(order);
	switch (size) {
	case 1:
		if (__atomic_always_lock_free(1, 0) && ((uintptr_t)object % 1U) == 0) {
			uint8_t value;

			memcpy(&value, desired, sizeof(value));
			__atomic_store_n((volatile uint8_t *)object, value, __ATOMIC_SEQ_CST);
			return;
		}
		break;
	case 2:
		if (__atomic_always_lock_free(2, 0) && ((uintptr_t)object % 2U) == 0) {
			uint16_t value;

			memcpy(&value, desired, sizeof(value));
			__atomic_store_n((volatile uint16_t *)object, value, __ATOMIC_SEQ_CST);
			return;
		}
		break;
	case 4:
		if (__atomic_always_lock_free(4, 0) && ((uintptr_t)object % 4U) == 0) {
			uint32_t value;

			memcpy(&value, desired, sizeof(value));
			__atomic_store_n((volatile uint32_t *)object, value, __ATOMIC_SEQ_CST);
			return;
		}
		break;
	case 8:
		if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
			uint64_t value;

			memcpy(&value, desired, sizeof(value));
			__atomic_store_n((volatile uint64_t *)object, value, __ATOMIC_SEQ_CST);
			return;
		}
		break;
	default:
		break;
	}
	k_spinlock_key_t key = atomic_lock_acquire();

	memcpy((void *)object, desired, size);
	atomic_lock_release(key);
}

void z_libcxx_atomic_exchange(size_t size, volatile void *object, void *desired, void *result,
			      int order)
{
	ARG_UNUSED(order);
	switch (size) {
	case 1:
		if (__atomic_always_lock_free(1, 0) && ((uintptr_t)object % 1U) == 0) {
			uint8_t value;

			memcpy(&value, desired, sizeof(value));
			value = __atomic_exchange_n((volatile uint8_t *)object, value,
						    __ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	case 2:
		if (__atomic_always_lock_free(2, 0) && ((uintptr_t)object % 2U) == 0) {
			uint16_t value;

			memcpy(&value, desired, sizeof(value));
			value = __atomic_exchange_n((volatile uint16_t *)object, value,
						    __ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	case 4:
		if (__atomic_always_lock_free(4, 0) && ((uintptr_t)object % 4U) == 0) {
			uint32_t value;

			memcpy(&value, desired, sizeof(value));
			value = __atomic_exchange_n((volatile uint32_t *)object, value,
						    __ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	case 8:
		if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
			uint64_t value;

			memcpy(&value, desired, sizeof(value));
			value = __atomic_exchange_n((volatile uint64_t *)object, value,
						    __ATOMIC_SEQ_CST);
			memcpy(result, &value, sizeof(value));
			return;
		}
		break;
	default:
		break;
	}
	k_spinlock_key_t key = atomic_lock_acquire();

	volatile uint8_t *target = object;
	const uint8_t *input = desired;
	uint8_t *output = result;

	/* Preserve the desired value when input and result are the same buffer. */
	for (size_t i = 0; i < size; ++i) {
		uint8_t replacement = input[i];
		uint8_t previous = target[i];

		target[i] = replacement;
		output[i] = previous;
	}
	atomic_lock_release(key);
}

bool z_libcxx_atomic_compare_exchange(size_t size, volatile void *object, void *expected,
				      void *desired, int success, int failure)
{
	ARG_UNUSED(success);
	ARG_UNUSED(failure);
	switch (size) {
	case 1:
		if (__atomic_always_lock_free(1, 0) && ((uintptr_t)object % 1U) == 0) {
			uint8_t value;
			uint8_t old;

			memcpy(&value, desired, sizeof(value));
			memcpy(&old, expected, sizeof(old));
			bool matched = __atomic_compare_exchange_n((volatile uint8_t *)object, &old,
								   value, false, __ATOMIC_SEQ_CST,
								   __ATOMIC_SEQ_CST);

			if (!matched) {
				memcpy(expected, &old, sizeof(old));
			}
			return matched;
		}
		break;
	case 2:
		if (__atomic_always_lock_free(2, 0) && ((uintptr_t)object % 2U) == 0) {
			uint16_t value;
			uint16_t old;

			memcpy(&value, desired, sizeof(value));
			memcpy(&old, expected, sizeof(old));
			bool matched = __atomic_compare_exchange_n(
				(volatile uint16_t *)object, &old, value, false, __ATOMIC_SEQ_CST,
				__ATOMIC_SEQ_CST);

			if (!matched) {
				memcpy(expected, &old, sizeof(old));
			}
			return matched;
		}
		break;
	case 4:
		if (__atomic_always_lock_free(4, 0) && ((uintptr_t)object % 4U) == 0) {
			uint32_t value;
			uint32_t old;

			memcpy(&value, desired, sizeof(value));
			memcpy(&old, expected, sizeof(old));
			bool matched = __atomic_compare_exchange_n(
				(volatile uint32_t *)object, &old, value, false, __ATOMIC_SEQ_CST,
				__ATOMIC_SEQ_CST);

			if (!matched) {
				memcpy(expected, &old, sizeof(old));
			}
			return matched;
		}
		break;
	case 8:
		if (__atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0) {
			uint64_t value;
			uint64_t old;

			memcpy(&value, desired, sizeof(value));
			memcpy(&old, expected, sizeof(old));
			bool matched = __atomic_compare_exchange_n(
				(volatile uint64_t *)object, &old, value, false, __ATOMIC_SEQ_CST,
				__ATOMIC_SEQ_CST);

			if (!matched) {
				memcpy(expected, &old, sizeof(old));
			}
			return matched;
		}
		break;
	default:
		break;
	}
	k_spinlock_key_t key = atomic_lock_acquire();

	bool matched = memcmp((const void *)object, expected, size) == 0;

	if (matched) {
		memcpy((void *)object, desired, size);
	} else {
		memcpy(expected, (const void *)object, size);
	}
	atomic_lock_release(key);
	return matched;
}

bool z_libcxx_atomic_is_lock_free(size_t size, const volatile void *object)
{
	switch (size) {
	case 1:
		return __atomic_always_lock_free(1, 0);
	case 2:
		return __atomic_always_lock_free(2, 0) && ((uintptr_t)object % 2U) == 0;
	case 4:
		return __atomic_always_lock_free(4, 0) && ((uintptr_t)object % 4U) == 0;
	case 8:
		return __atomic_always_lock_free(8, 0) && ((uintptr_t)object % 8U) == 0;
	default:
		return false;
	}
}
