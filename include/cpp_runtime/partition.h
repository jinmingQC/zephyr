/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CPP_RUNTIME_PARTITION_H_
#define CPP_RUNTIME_PARTITION_H_
#ifdef __cplusplus
extern "C" {
#endif
/* Ordinary archive data/bss anchors: deliberately NOT section-annotated.
 * Domain registration verifies the archive mapping actually captured both.
 */
extern unsigned char cpp_runtime_data_anchor;
extern unsigned char cpp_runtime_bss_anchor;
#ifdef __cplusplus
}
#endif
#endif
