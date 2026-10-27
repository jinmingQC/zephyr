.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

libc++ threading proof of concept
################################

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

The integration suite passed with GCC 14.3.0 and Clang 19.1.7 from Zephyr SDK
1.0.1 on all three QEMU targets below. Each compiler/target combination ran
all 22 test cases, for 132 successful case executions:

* ``mps2/an385`` (ARM Cortex-M3)
* ``qemu_riscv32/qemu_virt_riscv32`` (RISC-V, one core)
* ``qemu_riscv32/qemu_virt_riscv32/smp`` (RISC-V, two cores)

The new sample also passed with GCC using automatic source downloads. The
existing C++ hello-world sample passed with its default library selection.
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

A joined thread releases its pool slot. An exited detached thread is
reclaimed when a later thread is created, after the kernel confirms that it
has terminated. Merely returning from the application callback does not
permit immediate stack reuse. Exceeding the thread pool or allocation limits
can terminate the application because this profile disables exceptions.

Source acquisition
******************

On the first configuration of a build directory, CMake downloads the pinned
libc++ and libc++abi 19.1.7 release archives and verifies their SHA-256 hashes.
The hashes and release URLs are recorded in ``CMakeLists.txt``. Downloads and
generated headers remain in the build directory; upstream runtime files are
not copied into the Zephyr source tree.

For an offline build, unpack both matching release archives beforehand and
append these arguments to the ``west build`` CMake command:

.. code-block:: console

   -DLIBCXX_SOURCE_DIR=/path/to/libcxx-19.1.7.src
   -DLIBCXXABI_SOURCE_DIR=/path/to/libcxxabi-19.1.7.src

Both directories must contain the unmodified 19.1.7 sources. Local source
overrides bypass archive downloading and hash verification; the caller is
responsible for their provenance and matching versions.

Build and run
*************

Use an initialized west workspace with its required modules and Python
dependencies. From this Zephyr checkout, set the SDK path and explicitly
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

API coverage and limits
***********************

The test suite exercises thread creation, move, identity, join, detach, pool
reuse, sleep, mutex contention, recursive and timed mutexes, shared mutexes,
lock wrappers, condition-variable notifications and deadlines, ``call_once``,
local-static initialization, shared-pointer reference counting, futures,
promises, and ``async``. It also checks nontrivial ``thread_local`` destruction
and the ordering of ``notify_all_at_thread_exit()``. This suite is a targeted
integration test, not the complete upstream libc++ conformance suite.

The implementation has these limits:

* Only supervisor-mode builds are supported. ``CONFIG_USERSPACE=y`` is rejected.
  This adapter does not manage user-mode kernel-object permissions.
* Exceptions and RTTI are disabled. Operations that would throw on failure
  terminate instead. No exception unwinder is built.
* Filesystem, localization, C++ stream I/O, wide-character facilities,
  ``random_device``, time-zone databases, and C++ modules are outside this
  profile. This is not a complete hosted C++17 library.
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
