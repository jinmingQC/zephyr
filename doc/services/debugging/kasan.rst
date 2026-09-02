.. _kasan:

Kernel Address Sanitizer
########################

Zephyr provides a lightweight Kernel Address Sanitizer (KASAN) runtime for
debugging memory accesses on embedded targets. Unlike host ASAN, it does not
link a compiler runtime library. Instrumented code calls the Zephyr runtime,
which asks the enabled memory providers whether an access overlaps poisoned
memory.

The following providers are available:

* Heap KASAN tracks allocations in selected ``sys_heap`` instances with a
  compact bit array. It detects out-of-bounds and use-after-free reads and
  writes at the configured shadow granularity.
* Global KASAN uses compiler-generated redzones and descriptors around global
  and function-static objects. The descriptors provide exact object bounds
  and, when emitted by the compiler, the object name and source location.

Both providers share compiler callbacks, libc interceptors, diagnostics, and
the :c:func:`kasan_report` hook. They can be enabled together.

Configuration
*************

Enable one or both providers with :kconfig:option:`CONFIG_SYS_HEAP_KASAN` and
:kconfig:option:`CONFIG_KASAN_GLOBAL`. Read instrumentation is enabled by
default and can be disabled with
:kconfig:option:`CONFIG_KASAN_INSTRUMENT_READS` to reduce runtime overhead.

Global KASAN keeps a sorted pointer index for compiler descriptors. Set
:kconfig:option:`CONFIG_KASAN_GLOBAL_MAX_OBJECTS` to at least the number of
global and function-static objects defined by the instrumented translation
units. Initialization stops with a fatal error if the index is too small.

Heap KASAN also requires each protected heap to be registered. The common
malloc and system heaps can be selected with
:kconfig:option:`CONFIG_SYS_HEAP_KASAN_MALLOC` and
:kconfig:option:`CONFIG_SYS_HEAP_KASAN_SYSTEM`. Custom heaps use
``K_HEAP_KASAN_ENABLE()`` or ``SYS_HEAP_KASAN_ENABLE()``.

Selecting source files
**********************

KASAN instrumentation is opt-in. After adding all sources to a CMake target,
instrument either the complete target or a directory within its source list::

   target_sources(app PRIVATE
     src/main.c
     src/checked/parser.c
   )

   # Instrument every source in app.
   zephyr_target_enable_kasan(app)

   # Or instrument only sources below src/checked.
   zephyr_kasan_enable_directory(src/checked)

The older ``zephyr_target_enable_heap_kasan()`` and
``zephyr_heap_kasan_enable_directory()`` helpers remain available for Heap
KASAN-only applications.

Instrument code that accesses protected memory. Do not instrument the KASAN
runtime or a shadow provider; Zephyr excludes its own implementations from
instrumentation to prevent recursion.

Reporting
*********

The default report prints the access direction and size, the first poisoned
address, the caller, and provider-specific object information, then panics.
Applications can override :c:func:`kasan_report` to send the structured
``kasan_fault`` data to a crash collector. Heap KASAN's existing
``heap_kasan_report()`` override remains supported.

Limitations
***********

KASAN is intended for debug and validation builds. It increases code size,
memory usage, and execution time, especially when read instrumentation is
enabled.

Stack redzones and dynamic-initialization-order checking are not implemented.
Global descriptors become active when Zephyr runs GNU constructors, after
``POST_KERNEL`` initialization and before ``APPLICATION`` initialization.
Global accesses made at earlier init levels are not checked.

Global KASAN supports GCC and Clang in kernel-address mode. Clang can omit
source location metadata, in which case reports still contain the object name
and exact bounds.
