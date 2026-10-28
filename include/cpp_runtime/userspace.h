/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors */
#ifndef CPP_RUNTIME_USERSPACE_H_
#define CPP_RUNTIME_USERSPACE_H_

#include <zephyr/app_memory/mem_domain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the single C++ application memory domain before starting its user
 * threads. The caller must already have initialized the domain and must add
 * its application-data and libc malloc partitions separately.
 *
 * This privileged, thread-context-only API maps cpp_runtime_partition.
 * Repeated registration of the same domain succeeds; a second domain fails.
 * The domain must remain alive and unchanged while runtime threads use it.
 *
 * The shared libc++ instance is one application trust domain, not independent
 * process isolation. Privileged services must not consume user-controlled
 * C++ callbacks or objects from the shared instance during normal operation.
 *
 * Returns zero or a negative errno.
 */
int cpp_runtime_userspace_init(struct k_mem_domain *domain);

extern struct k_mem_partition cpp_runtime_partition;

#ifdef __cplusplus
}
#endif

#endif /* CPP_RUNTIME_USERSPACE_H_ */
