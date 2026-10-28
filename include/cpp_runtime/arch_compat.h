/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CPP_RUNTIME_ARCH_COMPAT_H_
#define CPP_RUNTIME_ARCH_COMPAT_H_

/*
 * Zephyr ec989188's RISC-V thread header declares this C implementation outside
 * extern "C". Establish its linkage before ANY Zephyr header, including when
 * applications include kernel.h before a C++ standard header.
 */
#if defined(__cplusplus) && defined(CONFIG_USERSPACE) && defined(CONFIG_RISCV)
extern "C" bool z_riscv_thread_is_user_mode(void);
#endif

#endif
