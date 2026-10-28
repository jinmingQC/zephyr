/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CPP_RUNTIME_FAILFAST_ABORT_H_
#define CPP_RUNTIME_FAILFAST_ABORT_H_

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

FUNC_NORETURN void __real_abort(void);
FUNC_NORETURN void __wrap_abort(void);

#ifdef __cplusplus
}
#endif
#endif
