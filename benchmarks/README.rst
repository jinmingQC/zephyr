.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
.. SPDX-License-Identifier: Apache-2.0

Thread and synchronization measurements
======================================

Both backend branches use the same application source, configuration and parser.
Compare the same Zephyr revision, libc++/libc++abi release, compiler, board, SMP
mode and optimization. The common profile requests eight 4096-byte worker stacks,
eight TLS keys, priority 5 for coordinator and workers, a 32768-byte C allocation
arena and an 8192-byte kernel heap. Exceptions and RTTI are disabled. Audit the
generated zephyr/.config: a requested setting does not prove its selected value.
Backend-required kernel options can differ: POSIX dynamic threads select
THREAD_MONITOR and THREAD_STACK_INFO. These are implementation costs of this
route, not application-resource policy mismatches; record them explicitly.

The applications
---------------

The minimal application returns from main without instantiating a C++ threading
object. It selects the same runtime profile as thread_sync. This measures
retained baseline configuration and registration costs, including backend
objects retained by Zephyr's static object sections. It does not force every
archive member into the image.

The thread_sync application measures six operations without ztest:

* mutex_uncontended: one warmed mutex lock/unlock pair, repeated 10000 times.
* mutex_first_use: construct, first lock/unlock and destroy a mutex, repeated
  1000 times. This exposes a backend that allocates an object lazily.
* mutex_contended: release two already-created waiting workers, run 2000
  protected increments per worker, and join both. The denominator is 4000
  increments. Gate release, scheduling and joins are included; this is a
  two-worker workload measurement rather than isolated lock latency. On UP,
  workers need not block on one another; actual contention is not forced.
* condition_round_trip: 200 predicate-based request/reply round trips between
  two threads. The worker is created and ready before timing; the final join
  is outside the timer.
* thread_create_join: create one worker, execute its atomic increment, and
  join it, repeated 32 times.
* call_once_ready: call an already-completed once flag 10000 times.

Each operation runs one discarded warmup batch and nine recorded batches.
Printing and correctness checks occur outside timed regions. The program checks
protected increments, replies, completions and exactly-once execution. A worker
created before measurement records its actual kernel priority; a mismatch with
the requested priority rejects the run. This matters because native workers use
a configured priority while POSIX workers inherit the creator's priority.

Timing and raw results
----------------------

BENCH_RAW CSV columns are metric, zero-based sample index, elapsed platform
timer-counter ticks, operation count and counter frequency in Hz. BENCH_SUMMARY
columns are metric, sample count, minimum ticks, median ticks, nearest-rank p90
ticks and integer median nanoseconds per operation. With nine samples,
nearest-rank p90 is the maximum. Inspect all raw samples as well as the median;
nine samples are not a confidence interval.

The timer is k_cycle_get_32(). Unsigned subtraction handles one wrap. An
independent uptime check with tick-rounding allowance rejects intervals that
could reach a full counter period. The log records counter and uptime-tick
frequencies. These are platform timer ticks, not retired CPU instructions.
Counter-read overhead remains in every measured batch without subtraction.

QEMU values are simulator observations. The counter may follow virtual time,
host wall time or an emulated clock depending on board and QEMU options. They
cannot establish physical hardware latency, energy use or a universal backend
speed advantage. Run one QEMU at a time with the same options; alternate backend
order between repeated runs. Repeat on the target board before a product
decision. An external runner must bound a hung benchmark; the Twister console
test has a 180-second timeout.

Validate complete console logs and generate JSON with::

   python3 scripts/parse_benchmark.py native.log posix.log -o benchmark.json

The parser requires successful completion, all six sets of nine samples, equal
coordinator/worker priorities and summaries matching the raw CSV. It rejects
truncated, failed or inconsistent runs. META records include compiler version,
C++ language mode, SMP mode and public synchronization type sizes. Type sizes
describe storage per object; they are not the full kernel resource cost.

Image size and RAM
------------------

Keep the minimal and thread_sync ELF files, linker maps, generated configuration
and section/memory reports. Conventionally image/ROM size is text plus data and static writable RAM is data plus bss.
RV32 QEMU uses a RAM-only linker layout, so its text also occupies RAM; data+bss
alone is not total board RAM. Inspect Zephyr's board-specific memory-region
report for actual residency, reserved alignment and other sections.
Stack arrays and allocator arenas already in bss must not be added again.

Compare each backend's absolute minimal image, absolute benchmark image and the
difference between applications. Archive garbage collection makes this
difference non-additive; report an observed image difference.

Static RAM is not peak dynamic consumption. This benchmark does not instrument
allocator or stack high-water marks, or peak live object count. Align the native
thread pool and POSIX dynamic stack pool at eight 4096-byte stack requests.
If POSIX instead takes stacks from a heap, report that policy and heap capacity
rather than treating smaller bss as less total memory. POSIX object-table
capacities and native per-object storage also need separate reporting.

Source line counts are only a maintenance-surface proxy. Separate the native
adapter, POSIX glue, required kernel patch and reused Zephyr POSIX implementation.
Common libc++ sources and module build configuration are shared costs.

Reproduce the matrix
--------------------

Preview all commands without building or running anything::

   python3 scripts/benchmark_matrix.py

Build every backend/compiler/platform image and retain ELF files outside Git::

   python3 scripts/benchmark_matrix.py --build-only --run

After compilation and all other QEMU jobs finish, measure existing images::

   python3 scripts/benchmark_matrix.py --measure-only --run

The default is GCC and Clang on ARM mps2/an385, RISC-V32 UP and RISC-V32 SMP2.
Three rounds alternate native/POSIX, POSIX/native and native/POSIX. Options can
select one backend, compiler or platform; --help documents workspace, SDK and
source overrides. Source and configuration bytes must match between branches.
The script audits selected configuration and application O2/C++17 flags, retains
full build logs/configuration/map/ELF locally, and exports small JSON reports with
source hashes, commands, section sizes and all raw samples. Compiling a selected
cell again appends its logs rather than deleting its earlier memory-region report.
