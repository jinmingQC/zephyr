/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>

/*
 * GCC 14 can drop libc++ 19's explicit constexpr string destructor instantiation
 * at -Os in C++23 mode, leaving locale.cpp with an unresolved ABI reference.
 * Compile this translation unit as C++17 so the same upstream destructor body
 * is instantiated without constexpr. The implementation and ABI remain libc++'s;
 * this creates only the ordinary weak template definitions.
 */
_LIBCPP_BEGIN_NAMESPACE_STD

template _LIBCPP_EXPORTED_FROM_ABI basic_string<char>::~basic_string();

_LIBCPP_END_NAMESPACE_STD
