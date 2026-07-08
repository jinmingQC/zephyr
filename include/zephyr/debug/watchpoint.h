/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Memory watchpoint API
 */

#ifndef ZEPHYR_INCLUDE_DEBUG_WATCHPOINT_H_
#define ZEPHYR_INCLUDE_DEBUG_WATCHPOINT_H_

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup watchpoint_apis Memory Watchpoint APIs
 * @ingroup kernel_apis
 * @{
 */

/** Trigger on read accesses. */
#define K_WATCHPOINT_READ BIT(0)

/** Trigger on write accesses. */
#define K_WATCHPOINT_WRITE BIT(1)

/** Trigger on read or write accesses. */
#define K_WATCHPOINT_RW (K_WATCHPOINT_READ | K_WATCHPOINT_WRITE)

struct k_watchpoint;

/** Trigger timing relative to the monitored memory access. */
enum k_watchpoint_timing {
	/** The backend cannot determine the access timing. */
	K_WATCHPOINT_TIMING_UNKNOWN,
	/** The callback runs before the monitored instruction retires. */
	K_WATCHPOINT_TIMING_BEFORE,
	/** The callback runs after the monitored instruction retires. */
	K_WATCHPOINT_TIMING_AFTER,
};

/** Information about an access that fired a watchpoint. */
struct k_watchpoint_event {
	/**
	 * Program counter reported by the architecture.
	 *
	 * For a before-access event this identifies the monitored instruction.
	 * For an after-access event this may identify the following instruction.
	 */
	void *pc;

	/**
	 * Hardware-reported access address.
	 *
	 * This field must only be used when @ref access_addr_valid is true.
	 */
	void *access_addr;

	/** Whether @ref access_addr contains a valid access address. */
	bool access_addr_valid;

	/** Access size in bytes, or 0 when unavailable. */
	size_t access_size;

	/**
	 * Access type.
	 *
	 * A backend may report @ref K_WATCHPOINT_RW when the hardware cannot
	 * distinguish which configured access type fired.
	 */
	uint32_t flags;

	/** Timing of the callback relative to the monitored access. */
	enum k_watchpoint_timing timing;

	/**
	 * Call stack captured at the watchpoint hit, or NULL when unavailable.
	 *
	 * The first entry is the reported PC when available. Additional entries
	 * depend on the architecture stack unwinder and exception context.
	 *
	 * Entries are valid only for the duration of the callback.
	 */
	const uintptr_t *callstack;

	/** Number of entries in @ref callstack. */
	size_t callstack_depth;
};

/**
 * @brief Watchpoint callback.
 *
 * The callback runs in synchronous exception context and may run
 * concurrently on multiple CPUs. It must be bounded and must not block.
 * Only exception-safe operations, such as atomics or lock-free state
 * capture, may be used. k_watchpoint_add() and k_watchpoint_remove() are
 * not allowed from this callback.
 *
 * A backend that cannot safely resume a before-access watchpoint
 * automatically deactivates it. Thread context can re-arm it after the
 * callback returns. Memory accesses made by the callback are not guaranteed
 * to be observed by other watchpoints.
 *
 * @param wp Watchpoint that fired.
 * @param event Triggering access information.
 * @param arg User-provided argument.
 */
typedef void (*k_watchpoint_cb_t)(const struct k_watchpoint *wp,
				  const struct k_watchpoint_event *event,
				  void *arg);

/**
 * @brief Watchpoint descriptor.
 *
 * Zero-initialize the descriptor or use @ref K_WATCHPOINT_INITIALIZER or
 * @ref K_WATCHPOINT_DEFINE. The descriptor and monitored address must remain
 * valid while the watchpoint is active. Do not modify descriptor fields until
 * k_watchpoint_remove() returns or k_watchpoint_is_active() reports false for
 * an automatically deactivated watchpoint.
 */
struct k_watchpoint {
	/** Start address of the monitored region. Address 0 is valid. */
	void *addr;

	/** Size of the monitored region in bytes. */
	size_t size;

	/** K_WATCHPOINT_READ, K_WATCHPOINT_WRITE, or K_WATCHPOINT_RW. */
	uint32_t flags;

	/** Callback invoked when the watchpoint fires. */
	k_watchpoint_cb_t cb;

	/** Argument passed to @ref cb. */
	void *arg;

	/** @cond INTERNAL_HIDDEN */
	atomic_t _state;
	/** @endcond */
};

/** Initialize an inactive watchpoint descriptor. */
#define K_WATCHPOINT_INITIALIZER(_addr, _size, _flags, _cb, _arg) \
	{                                                          \
		.addr = (_addr),                                    \
		.size = (_size),                                    \
		.flags = (_flags),                                  \
		.cb = (_cb),                                        \
		.arg = (_arg),                                      \
		._state = ATOMIC_INIT(0),                           \
	}

/** Statically define an inactive watchpoint. */
#define K_WATCHPOINT_DEFINE(name, _addr, _size, _flags, _cb, _arg) \
	struct k_watchpoint name =                                     \
		K_WATCHPOINT_INITIALIZER(_addr, _size, _flags, _cb, _arg)

/**
 * @brief Arm a watchpoint.
 *
 * The architecture must not widen the requested region. Whether an access
 * that only overlaps the region is detected is hardware-dependent. Hardware
 * may suppress matches while handling exceptions or with interrupts disabled.
 *
 * On SMP systems, success means the watchpoint is installed on every online
 * CPU before this function returns.
 *
 * This function must be called from thread context with interrupts enabled.
 *
 * @retval 0 Success.
 * @retval -EINVAL Invalid descriptor or overflowing address range.
 * @retval -EBUSY The descriptor is active or is being updated.
 * @retval -EWOULDBLOCK Called with interrupts disabled or from exception context.
 * @retval -ENOSPC No suitable hardware resources are available.
 * @retval -ENOTSUP The request cannot be represented by the backend.
 */
int k_watchpoint_add(struct k_watchpoint *wp);

/**
 * @brief Disarm a watchpoint.
 *
 * This function waits for callbacks already in progress. On SMP systems,
 * success means the watchpoint is removed from every online CPU. Removing an
 * inactive watchpoint succeeds.
 *
 * This function must be called from thread context with interrupts enabled.
 *
 * @retval 0 Success.
 * @retval -EINVAL @p wp is NULL.
 * @retval -EBUSY The descriptor is being added or removed.
 * @retval -EWOULDBLOCK Called with interrupts disabled or from exception context.
 * @retval -ENOTSUP A CPU could not update its debug hardware.
 */
int k_watchpoint_remove(struct k_watchpoint *wp);

/**
 * @brief Test whether a watchpoint is active.
 *
 * This is a snapshot. The state can change immediately after the call.
 *
 * @param wp Watchpoint descriptor.
 *
 * @retval true The watchpoint is active or being removed.
 * @retval false The watchpoint is inactive or @p wp is NULL.
 */
bool k_watchpoint_is_active(const struct k_watchpoint *wp);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DEBUG_WATCHPOINT_H_ */
