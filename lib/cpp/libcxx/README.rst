.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

libc++ C++17 proof of concept
############################

For a Chinese introduction, architecture diagrams and hands-on experiments,
see `the Chinese guide <README.zh-CN.md>`_.

This optional library configuration builds libc++ and libc++abi 19.1.7 with
Zephyr's selected target compiler and picolibc. Applications use C++17; the
runtime sources use C++23, as required by LLVM 19. GCC 14 or newer and Clang 17
or newer are required. Compiler binaries, installed SDK libraries, and sysroot
headers are not modified.

The external threading interface in ``include/__external_threading`` maps
libc++ operations to Zephyr mutexes, condition variables, threads, and
thread-local storage. It does not use pthreads. The same generated libc++
configuration is used by applications and both runtime libraries. Compiler
default C++ include paths and prebuilt C++ runtime libraries are excluded.

The integration suites cover threading, time, filesystem, exceptions and RTTI,
containers and atomics, streams, random-device routing, and allocation-failure
recovery. They are exercised with GCC 14.3.0 and Clang 19.1.7 from Zephyr SDK
1.0.1 on these QEMU targets:

* ``mps2/an385`` (ARM Cortex-M3)
* ``qemu_riscv32/qemu_virt_riscv32`` (RISC-V, one core)
* ``qemu_riscv32/qemu_virt_riscv32/smp`` (RISC-V, two cores)

The final validation set contains nine feature profiles per compiler and
target: 54 configurations and 1086 successful case executions. The profiles
cover 22 threading, 16 time, 43 filesystem, 18 standard-library/atomic,
26 exception/RTTI, 22 stream, 22 stream-with-exception, 7 random-device,
and 5 filesystem-allocation cases per compiler/target.

Four additional Cortex-M3 configurations check exceptions-only and RTTI-only
application settings with both compilers, adding 72 successful case executions.
The final set therefore has 58 configurations and 1158 passing cases.
Additional diagnostic and repeated runs are excluded from these totals.
The strengthened GCC atomic suite also passed on all three targets.
Custom compiler modifications and physical hardware require separate validation.

Configuration
*************

An application can select this implementation with:

.. code-block:: cfg

   CONFIG_CPP=y
   CONFIG_STD_CPP17=y
   CONFIG_PICOLIBC=y
   CONFIG_LIBCXX_SOURCE_LIBCPP=y
   CONFIG_CPP_EXCEPTIONS=n
   CONFIG_CPP_RTTI=n
   CONFIG_POSIX_SYSTEM_INTERFACES=y
   CONFIG_POSIX_TIMERS=y
   CONFIG_POSIX_MONOTONIC_CLOCK=y
   CONFIG_COMMON_LIBC_MALLOC=y
   CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=32768

Native thread-local storage is required and selected by the library. POSIX
clock support supplies ``clock_gettime()`` for the upstream chrono
implementation. It does not select the POSIX pthread implementation.

Filesystem support is optional. In addition to the configuration above, enable:

.. code-block:: cfg

   CONFIG_FILE_SYSTEM=y
   CONFIG_LIBCXX_FILESYSTEM=y
   CONFIG_LIBCXX_FILESYSTEM_TEMP_DIR="/lfs/tmp"

Select and configure a native Zephyr filesystem and its storage device
separately. The filesystem tests use LittleFS on a RAM disk; their
``prj.conf`` and ``app.overlay`` provide a complete QEMU example. Applications
must mount their storage before calling filesystem APIs. The configured
temporary directory must be absolute, mounted, and already created.

Exceptions and RTTI can be enabled independently on supported targets:

.. code-block:: cfg

   CONFIG_CPP_EXCEPTIONS=y
   CONFIG_CPP_RTTI=y

The exception backend is enabled for ARM and RISC-V. It builds LLVM
libunwind from source and uses Zephyr's retained unwind tables. Exception
objects, library calls, and nested stack unwinding need additional heap and
stack space; the exception tests provide one tested configuration.
The standard-library suite also provides ``exceptions.conf`` and ``rtti.conf``
for checking the feature switches independently with ``EXTRA_CONF_FILE``.

For narrow stream I/O, add:

.. code-block:: cfg

   CONFIG_LIBCXX_STREAMS=y
   CONFIG_PICOLIBC_IO_FLOAT=y

This option also requires the filesystem configuration above. For
``std::random_device``, enable ``CONFIG_LIBCXX_RANDOM_DEVICE=y`` and configure
a Zephyr cryptographically secure random source. An ordinary PRNG or clock
is not used as a fallback. The deterministic test generator is allowed only
in explicit ZTEST builds.

The default resource limits are:

.. list-table::
   :header-rows: 1

   * - Kconfig option
     - Default
     - Meaning
   * - ``CONFIG_LIBCXX_THREAD_MAX``
     - 8
     - Thread objects and stacks in the fixed pool
   * - ``CONFIG_LIBCXX_THREAD_STACK_SIZE``
     - 4096
     - Requested bytes per thread stack
   * - ``CONFIG_LIBCXX_THREAD_PRIORITY``
     - 5
     - Preemptible Zephyr priority for new threads
   * - ``CONFIG_LIBCXX_TLS_KEYS``
     - 8
     - Internal runtime thread-specific storage keys

The priority must be less than ``CONFIG_NUM_PREEMPT_PRIORITIES``. Stack and
heap sizes must be adjusted for the application. Thread-specific keys are
internal runtime resources, separate from language-level ``thread_local``
objects.

A joined thread releases its pool slot. Detaching an already-terminated
thread can also release its slot immediately. Otherwise an exited detached
thread is reclaimed when a later thread is created, after the kernel
confirms that it has terminated. Merely returning from the application callback does not
permit immediate stack reuse. Exceeding the thread pool or allocation limits
raises ``std::system_error`` or ``std::bad_alloc`` when exceptions are enabled;
without exceptions it terminates the application.

Source acquisition
******************

On the first configuration of a build directory, CMake downloads the pinned
libc++ and libc++abi 19.1.7 release archives and verifies their SHA-256 hashes.
Enabling exceptions also downloads matching libunwind sources. The hashes
and release URLs are recorded in ``CMakeLists.txt`` and ``unwind.cmake``.
Downloads and generated headers remain in the build directory; installed
SDK libraries and upstream source directories remain unchanged.

For an offline build, unpack the matching release archives beforehand and
append these arguments to the ``west build`` CMake command:

.. code-block:: console

   -DLIBCXX_SOURCE_DIR=/path/to/libcxx-19.1.7.src
   -DLIBCXXABI_SOURCE_DIR=/path/to/libcxxabi-19.1.7.src

For an exception-enabled offline build, also pass
``-DLIBUNWIND_SOURCE_DIR=/path/to/libunwind-19.1.7.src``.
All directories must contain the unmodified matching 19.1.7 sources. Local source
overrides bypass archive downloading and hash verification; the caller is
responsible for their provenance and matching versions.

Build and run
*************

Use an initialized west workspace with its required modules and Python
dependencies. Match the LittleFS checkout to this checkout's ``west.yml``;
older modules can reference the removed internal ``__ASSERT_ON`` macro.
An isolated matching module can be supplied without changing a shared
workspace with ``-DEXTRA_ZEPHYR_MODULES=/path/to/matching-littlefs``
(or Twister ``-x EXTRA_ZEPHYR_MODULES=/path/to/matching-littlefs``).
From this Zephyr checkout, set the SDK path and explicitly
select this checkout, including when using a Git worktree:

.. code-block:: console

   export ZEPHYR_BASE="$PWD"
   export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
   export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

For the small demonstration application, use ``samples/cpp/libcxx_threads``
in place of the test directory below. It starts four threads, synchronizes
their start with a condition variable, and prints ``libc++ threads: 4000 increments``.

Build and run the GCC test image:

.. code-block:: console

   west build -p always -b mps2/an385 -d build/libcxx-gcc \
     tests/lib/cpp/libcxx_threads -- -DTOOLCHAIN_VARIANT_COMPILER=gnu
   west build -d build/libcxx-gcc -t run

Use a separate build directory for Clang:

.. code-block:: console

   west build -p always -b mps2/an385 -d build/libcxx-clang \
     tests/lib/cpp/libcxx_threads -- -DTOOLCHAIN_VARIANT_COMPILER=llvm
   west build -d build/libcxx-clang -t run

Twister builds and executes the same suite and writes machine-readable
reports. Select the compiler in the toolchain environment as well as the
CMake arguments so that Twister's initial toolchain discovery agrees with the
build:

.. code-block:: console

   ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu west twister \
     -p mps2/an385 -T tests/lib/cpp/libcxx_threads \
     -O twister-libcxx-gcc -i -x TOOLCHAIN_VARIANT_COMPILER=gnu
   ZEPHYR_TOOLCHAIN_VARIANT=zephyr/llvm west twister \
     -p mps2/an385 -T tests/lib/cpp/libcxx_threads \
     -O twister-libcxx-clang -i -x TOOLCHAIN_VARIANT_COMPILER=llvm

For offline Twister runs, add ``-x LIBCXX_SOURCE_DIR=/path/to/libcxx-19.1.7.src``
and ``-x LIBCXXABI_SOURCE_DIR=/path/to/libcxxabi-19.1.7.src`` to each command.
``qemu_riscv32`` is an additional architecture to validate using the same
commands with a different board and output directory.

To run all suites on all three targets, use a distinct output directory for
each compiler:

.. code-block:: console

   for compiler in gnu llvm; do
     ZEPHYR_TOOLCHAIN_VARIANT=zephyr/$compiler west twister \
       -p mps2/an385 \
       -p qemu_riscv32/qemu_virt_riscv32 \
       -p qemu_riscv32/qemu_virt_riscv32/smp \
       -T tests/lib/cpp/libcxx_threads \
       -T tests/lib/cpp/libcxx_time \
       -T tests/lib/cpp/libcxx_filesystem \
       -T tests/lib/cpp/libcxx_exceptions \
       -T tests/lib/cpp/libcxx_standard \
       -T tests/lib/cpp/libcxx_streams \
       -T tests/lib/cpp/libcxx_random \
       -T tests/lib/cpp/libcxx_filesystem_alloc \
       -O twister-libcxx-all-$compiler -i \
       -x TOOLCHAIN_VARIANT_COMPILER=$compiler
   done

API coverage and limits
***********************

The threading suite exercises thread creation, move, identity, join, detach, pool
reuse, sleep, mutex contention, recursive and timed mutexes, shared mutexes,
lock wrappers, condition-variable notifications and deadlines, ``call_once``,
local-static initialization, shared-pointer reference counting, futures,
promises, and ``async``. It also checks nontrivial ``thread_local`` destruction
and the ordering of ``notify_all_at_thread_exit()``. This suite is a targeted
integration test, not the complete upstream libc++ conformance suite.

Time
====

``std::chrono::system_clock`` uses Zephyr's ``CLOCK_REALTIME`` and
``std::chrono::steady_clock`` uses ``CLOCK_MONOTONIC`` through the existing
upstream libc++ implementation. Duration arithmetic, time-point conversions,
``system_clock::to_time_t()`` and ``from_time_t()``, and the adapter's sleep and
timed synchronization APIs are available. This extension adds validation of
those paths; it does not replace the kernel's clock implementation.

The 16 tests in ``tests/lib/cpp/libcxx_time`` cover duration rounding,
time-point arithmetic, native clock correspondence, monotonic elapsed time,
zero and expired sleep deadlines, future steady and system clock deadlines,
condition-variable deadlines, and ``time_t`` conversions. They also step
realtime forward and backward while checking steady time, and exercise
``std::time()``, ``std::difftime()``, UTC conversion with ``std::gmtime()`` and
``std::strftime()``, and a ``std::localtime()``/``std::mktime()`` round trip.
Each test restores realtime, including elapsed monotonic time.

Applications must initialize realtime from an RTC, network service, or other
source when they need calendar time. Clock resolution and timeout granularity
follow the Zephyr configuration. The tests do not establish persistent RTC
support, a time-zone database, or CPU-time accounting through ``std::clock()``.
A realtime step does not change ``steady_clock``. Existing absolute
condition-variable waits still have the realtime-adjustment limitation below.

Filesystem
==========

With ``CONFIG_LIBCXX_FILESYSTEM=y``, the C++17 ``std::filesystem`` API uses a
native Zephyr ``fs_*`` backend. Upstream libc++ supplies paths and directory
iteration; the adapter supplies native file operations and per-iterator
directory state. It does not depend on POSIX pthreads or fabricated POSIX
inode and timestamp values.

Supported operations include:

* Path decomposition, composition, and lexical operations.
* ``absolute()``, ``canonical()``, ``weakly_canonical()``, and the related
  relative-path helpers.
* ``current_path()`` and the configured ``temp_directory_path()``.
* File type, existence, size, emptiness, and filesystem space queries.
* Directory creation, including ``create_directories()``; file resizing;
  renaming within one mount; and ``remove()``/``remove_all()``.
* ``directory_iterator``, ``recursive_directory_iterator``, and
  ``directory_entry`` type and size queries.
* File-content copying with ``copy_file()`` and file or directory copying
  with ``copy()``, including recursive directory copies.
* ``equivalent()`` and the identity checks required for safe copying, with
  the filesystem restriction below.

Identity comparisons across distinct resolved paths are implemented only
for LittleFS. They use the actual mounted filesystem type, filesystem
instance, and volume-relative path; they do not assume two different
spelled paths identify different files. Copying therefore requires both
source and destination to be on LittleFS. Other native filesystems can
provide basic operations, but identity-dependent operations can return
``ENOTSUP``. They need separate validation before an application relies on
them. The adapter does not provide hard links or symbolic links.

``copy_file()`` supports default fail-if-existing, ``skip_existing``, and
``overwrite_existing`` behavior. Copies transfer contents and use the
destination filesystem's default attributes. ``copy()`` without options
copies a directory and its immediate regular files; ``recursive`` also
copies its subdirectories. ``directories_only`` can be combined with
``recursive``. Copying a file onto itself or a directory into itself or its
descendants is rejected before output is truncated or the destination root
is created. Recursive copy and removal use explicit work lists instead of
unbounded C++ call-stack recursion.

The native filesystem API does not expose the metadata needed for
``last_write_time()``, ``permissions()``, or ``hard_link_count()``. These
operations report ``ENOTSUP`` for an existing path. Link creation, link
copying and ``read_symlink()``, and ``copy_options::update_existing`` also
report unsupported-operation errors. ``status().permissions()`` is
``perms::unknown``. ``create_directory(destination, attributes_from)``
validates that the source is a directory and creates the destination with
native defaults; it does not copy permissions. This is a practical subset
of C++17 filesystem semantics, not full filesystem conformance.

The adapter maintains its own global current directory, initially ``/``.
Changing it affects relative paths passed to this libc++ backend; it does
not change the working directory of native Zephyr or POSIX file APIs.
Prefer absolute paths when mixing APIs or sharing work across threads.
Mount-point parents must exist in the native namespace. Resolving a nested
mount does not synthesize missing intermediate directories.
``CONFIG_LIBCXX_FILESYSTEM_PATH_MAX`` defaults to 512 bytes and bounds path
storage used by native operations, including the terminating null byte.
Individual components must fit in the native ``fs_dirent::name`` storage;
oversized components report ``ENAMETOOLONG`` before a native operation.

``remove_all()`` refuses ``/`` and mounted roots before descending into
them. A removal can still be partial if a later child operation fails.
Copying is also nontransactional and can leave a partial destination on an
I/O error. C++ copy and recursive removal calls are serialized with each
other. Applications must coordinate other namespace mutations, including
native operations, concurrent renames, and mount or unmount operations;
identity checks do not hold a filesystem-wide lock.

Use overloads accepting ``std::error_code`` for recoverable filesystem
errors. With exceptions enabled, native metadata operations translate
internal allocation failures into ``ENOMEM`` when their interface requires
nonthrowing error reporting. Operations whose standard interface permits
allocation exceptions can still throw ``std::bad_alloc``. Locks and native
directory handles are released during stack unwinding; failure-injection
tests exercise both iterator construction and recursive copy.

With exceptions disabled, a failed throwing overload or allocation
exhaustion terminates the application. Work lists, path strings, and
iterators allocate memory. Size heaps for the directory trees being processed.
Optional C++ file streams use the same path resolver and current directory.

Run ``tests/lib/cpp/libcxx_time`` and ``tests/lib/cpp/libcxx_filesystem``
with the same compiler and board commands as the threading suite, using
separate output directories. Twister accepts repeated ``-T`` options to
select multiple suites. The filesystem suite provisions its own RAM-backed
LittleFS volume; applications must supply their own storage and mount setup.

Exceptions, RTTI, and atomics
============================

libc++abi supplies throwing, catching, type matching, ``exception_ptr``,
``nested_exception``, and RTTI. libunwind restores stack frames and runs
cleanup through ARM EHABI or RISC-V DWARF tables. The ARM runtime uses the
bare-metal ABI for type-table relocations. RISC-V uses an optional indexed
unwind table when the linker supports it and otherwise scans ``.eh_frame``.
Neither path requires pthreads or a rebuilt compiler. The ABI implementation
compiles the runtime C++ sources with internal RTTI even when application
RTTI is disabled.

Tests cover primitive and class exceptions, base-class matching, rethrow,
RAII destruction, failed constructors, multiple and virtual inheritance,
``dynamic_cast``, ``typeid``, library error types, concurrent exceptions,
``call_once`` retry, static initialization retry, and propagation through
promises, futures, ``async``, and packaged tasks. An uncaught exception in a
``std::thread`` callback still calls ``std::terminate``; it does not travel
automatically to the creating thread.

Unwinding is supported within normal C++ thread execution in a static
firmware image. Do not throw through an ISR, kernel boundary, or C frame
without unwind metadata. The unwinder uses native Zephyr locks and avoids
heap allocation in its frame cache. Dynamic/JIT/LLEXT frame registration is
outside this profile. Exception allocation still has finite resources;
out-of-memory recovery cannot be guaranteed after all emergency storage
is exhausted.

The atomic backend supplies missing 64-bit and generic object operations.
Aligned operations use compiler-native atomics when the target guarantees
they are lock-free. Other sizes and alignments share a native spinlock with
full ordering. Tests exercise compare-exchange expected-value updates,
carry/borrow, bitwise operations, no-tear loads, and concurrent 12-byte
aggregate updates. A fallback atomic is not lock-free and copies bytes
while holding the lock with interrupts masked. Avoid large objects and
latency-sensitive operations that would use this fallback.

Clang builds select libc++'s existing GNU-intrinsic atomic backend for both
runtime and application translation units. This avoids the pinned Clang 19
C11 aggregate lowering issue that pads a 12-byte value to 16 bytes and can
omit expected-value updates after failed compare-exchange. Tests check the
shared size/alignment and both strong and weak failure paths. The selected
atomic backend is part of this profile's ABI.

Streams and locale
==================

``CONFIG_LIBCXX_STREAMS`` builds upstream narrow iostreams, string streams,
formatting, locale facets, regular expressions, and file buffers. File
streams use picolibc's real ``funopen`` FILE implementation with native
``fs_open/read/write/seek/close`` callbacks. A generated ``fstream`` header
adapts the open boundary; it does not assume Zephyr POSIX descriptor objects
are compatible with picolibc's FILE layout.

The tests cover formatted and unformatted I/O, stream state and exceptions,
string streams, classic locale, regex, file creation and truncation,
append, seek, move, relative paths, flush visibility, and cleanup on failed
opens. Each buffered write calls ``fs_sync`` because picolibc provides no
separate flush callback for this bridge. This makes flush visible to other
handles but adds synchronization cost to writes.

Locale availability follows picolibc; the classic C locale is tested.
This configuration does not promise named regional locales or wide-character
streams. On the tested 32-bit targets, the pinned libc++ newlib interface
uses a 32-bit ``streamoff``. File offsets must also fit the native filesystem's
limits. Closing or flushing files can fail; applications should check stream
state or enable the appropriate stream exceptions.

Random sources and ordinary library facilities
==============================================

``std::random_device`` requests bytes from ``sys_csrand_get()``. Its default
constructor token is supported; unknown tokens and native failures raise
``std::system_error`` when exceptions are enabled. ``entropy()`` returns zero
because the native API does not report an entropy estimate. Tests inject
known bytes and errors at the native boundary and exercise concurrent calls;
they do not measure hardware entropy quality. Pseudorandom engines and
distributions are available independently of this option.

The standard-library suite also covers sequential and associative
containers, algorithms, numeric utilities, ``string_view``, ``optional``,
``variant``, ``any``, smart pointers, aligned allocation, polymorphic memory
resources, integer character conversion, and float/double formatting. The pinned
``long double`` formatting overloads narrow to ``double``; they do not
preserve wider long-double precision. Execution-policy algorithms use the
selected serial backend and do not promise parallel execution. These
are targeted integration checks, not the full upstream conformance suite.

Build-tree compatibility adaptations
===================================

The runtime and applications share generated headers and configuration.
The adaptations keep the upstream licenses and do not modify the source
archives or SDK. In addition to the OS boundaries, the pinned string header
bounds capacity by its iterator difference type. GCC-specific copies
initialize a hash-table temporary and scope false-positive diagnostics to
two split-buffer helpers, two deque insertion helpers, and the string
destructor. The quoted-input helper
initializes its temporary. The generated directory iterator releases its
native handle if construction throws.

GCC 14 can omit the exported string destructor when optimizing the upstream
C++23 explicit instantiation. A small GNU-only translation unit instantiates
the same header destructor in C++17 mode; the rest of the runtime keeps its
required C++23 mode. These compatibility changes are version-specific and
must be reevaluated when the pinned runtime or compiler changes.

General limits
==============

The implementation has these limits:

* Compiler-native lock-free 1-, 2-, and 4-byte atomic operations are required.
  Cortex-M0 is excluded; the build diagnoses this requirement explicitly.
* Only supervisor-mode builds are supported. ``CONFIG_USERSPACE=y`` is rejected.
  This adapter does not manage user-mode kernel-object permissions.
* Exceptions, RTTI, streams, and ``random_device`` are optional and disabled
  in the small default profile. Filesystem support has the subset above.
  Wide-character streams, Unicode conversion, time-zone databases, and C++
  modules are outside this profile. This is not a complete hosted C++17 library.
* The pinned libc++ 19 supports integral ``from_chars`` and floating
  ``to_chars``, but lacks floating ``from_chars``. Upstream adds float/double
  parsing in libc++ 20; upgrading the source runtime is separate from
  changing the compiler or SDK. Long-double parsing needs separate assessment.
* Zephyr native threads can use ordinary synchronization operations,
  ``this_thread::get_id()``, and create ``std::thread`` instances. Automatic
  runtime exit cleanup is provided only for threads created by this adapter.
  Do not use nontrivial ``thread_local`` destruction, exit notifications, or
  future readiness at native-thread exit as though cleanup were available.
* Language TLS destructors run before libc++ thread-specific destructors on
  an adapter-created thread. This ensures exit notifications and associated
  future readiness follow language TLS destruction. Global/static object
  destruction retains Zephyr's existing limitations.
* Thread termination must follow the normal callback return and join/detach
  lifecycle. Aborting an adapter thread through ``k_thread_abort()`` is
  unsupported and can bypass cleanup or retain a pool slot.
* Absolute condition-variable deadlines use ``CLOCK_REALTIME`` and are
  converted to relative kernel waits. Changes to realtime during a wait do
  not immediately update its timeout. Applications must keep realtime stable
  while relying on these waits. ``system_clock`` also needs the platform's
  normal clock initialization to represent wall time.
* ``hardware_concurrency()`` may return zero, which denotes an unknown count.
  Stack size and scheduling priority are fixed by configuration; the standard
  thread constructor has no Zephyr-specific attributes.

The runtime's external threading types form part of this libc++ build's ABI.
All C++ code linked into an image must use its generated configuration and
headers. Do not mix objects built against this implementation with objects
that expose incompatible libstdc++ or prebuilt SDK libc++ types.
