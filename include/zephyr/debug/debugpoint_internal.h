/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DEBUG_DEBUGPOINT_INTERNAL_H_
#define ZEPHYR_INCLUDE_DEBUG_DEBUGPOINT_INTERNAL_H_

#include <zephyr/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum z_debugpoint_type {
	Z_DEBUGPOINT_WATCH_READ,
	Z_DEBUGPOINT_WATCH_WRITE,
	Z_DEBUGPOINT_WATCH_RW,
	Z_DEBUGPOINT_BREAKPOINT,
	Z_DEBUGPOINT_TYPE_COUNT,
};

enum z_debugpoint_timing {
	Z_DEBUGPOINT_TIMING_UNKNOWN,
	Z_DEBUGPOINT_TIMING_BEFORE,
	Z_DEBUGPOINT_TIMING_AFTER,
};

struct z_debugpoint_handle {
	uint32_t generation;
	int slot;
};

struct arch_esf;
struct z_debugpoint;

struct z_debugpoint_event {
	void *pc;
	void *access_addr;
	bool access_addr_valid;
	size_t access_size;
	enum z_debugpoint_type type;
	enum z_debugpoint_timing timing;
	const struct arch_esf *esf;
};

typedef void (*z_debugpoint_cb_t)(const struct z_debugpoint *dp,
				  const struct z_debugpoint_event *event,
				  void *arg);
typedef void (*z_debugpoint_deactivate_t)(void *arg);

/*
 * Consumer-independent description of one logical hardware debugpoint.
 * owner uniquely identifies one frontend object, which may own one core slot.
 * deactivate notifies that frontend when a backend makes a point one-shot.
 */
struct z_debugpoint {
	enum z_debugpoint_type type;
	void *addr;
	size_t size;
	z_debugpoint_cb_t cb;
	z_debugpoint_deactivate_t deactivate;
	void *arg;
	void *owner;
	struct z_debugpoint_handle handle;
};

int z_debugpoint_add(const struct z_debugpoint *dp);
int z_debugpoint_remove(void *owner);
bool z_debugpoint_in_callback(void);

/*
 * Report a hardware hit. If deactivate is true, the backend must already have
 * disabled this logical point on the current CPU and marked it inactive in
 * backend state.
 */
void z_debugpoint_hit(struct z_debugpoint_handle handle,
		      const struct z_debugpoint_event *event, bool deactivate);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DEBUG_DEBUGPOINT_INTERNAL_H_ */
