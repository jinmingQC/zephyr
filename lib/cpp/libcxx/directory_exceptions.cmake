# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# The POSIX directory constructor owns a raw handle before allocating its first
# entry. Its destructor cannot run if construction throws, so release that
# handle before propagating an allocation failure.
set(iterator_constructor_tail [=[
    advance(ec);
  }

  ~__dir_stream() noexcept {
]=])
set(iterator_constructor_guard [=[
#if !defined(_LIBCPP_HAS_NO_EXCEPTIONS)
    try {
      advance(ec);
    } catch (...) {
      close();
      throw;
    }
#else
    advance(ec);
#endif
  }

  ~__dir_stream() noexcept {
]=])
string(REPLACE "${iterator_constructor_tail}" "${iterator_constructor_guard}"
  guarded_iterator_source "${native_iterator_source}"
)
if(guarded_iterator_source STREQUAL native_iterator_source)
  message(FATAL_ERROR "The pinned libc++ directory constructor has changed")
endif()
set(native_iterator_source "${guarded_iterator_source}")
