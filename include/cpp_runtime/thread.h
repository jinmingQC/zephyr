/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CPP_RUNTIME_THREAD_H_
#define CPP_RUNTIME_THREAD_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the calling thread's registered C++ TLS and library TSS destructors.
 *
 * Standard threads call this automatically on normal return. A native root
 * thread using this runtime must call it after releasing all C++ automatic
 * objects, immediately before returning. Never call it on another thread's
 * behalf or from a fault/abort hook. Do not use C++ runtime objects afterwards.
 */
void cpp_runtime_thread_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif
