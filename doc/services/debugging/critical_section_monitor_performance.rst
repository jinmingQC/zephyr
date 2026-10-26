.. SPDX-FileCopyrightText: Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
.. SPDX-License-Identifier: Apache-2.0

.. _critical_section_monitor_measurements:

Critical Section Monitor measurements
#####################################

These target-hardware measurements compare the same workloads with the monitor
disabled and enabled. They cover the ordinary path: completed intervals do not
replace existing maximum records. Clock reads and hook calls are included in
the reported overhead.

With a separately tested ESP32-C3 timer optimization, an empty IRQ lock/unlock
pair adds about 1.37 us and an uncontended spinlock pair adds about 1.68 us.
That timer patch is not part of the measured review revision. The original
ESP32-C3 timer gives 1.93 us and 2.26 us, respectively; JH7110 S7 gives
0.244 us and 0.326 us. These are single-core microbenchmarks, not application-wide
slowdowns or worst-case latency bounds.

Setup
*****

ESP32-C3 uses revision ``2edd03aedef8cb00bf8709439af752d68bfb8671``;
S7 uses ``8414a073013e8896cedc05cfa2181da202e24cc6``. The production monitor
source is identical in both revisions. ESP32-C3 component estimates use a
separate local copy with profiling probes; total overhead uses the unmodified
monitor.

Both targets use Zephyr SDK 1.0.1, GCC 14.3.0 and size optimization (``-Os``).
SMP, assertions, spinlock validation, spinlock time-limit checking, power
management, logging and shell are disabled. The monitor tracks up to four held
spinlocks. Original timer results and the ESP32-C3 timer-only optimization are
reported separately.

.. list-table:: Runtime configurations
   :header-rows: 1
   :widths: 24 38 38

   * - Item
     - ESP32-C3
     - JH7110 S7
   * - Hardware
     - FoloToy AI Passport, ESP32-C3 rev 1.1
     - StarFive VisionFive 2, physical hart 0
   * - Build target
     - ``esp32c3_devkitm/esp32c3``
     - ``visionfive2/jh7110``, UP on S7
   * - Compiled integer ISA
     - RV32IMC
     - RV64IMAC
   * - CPU counter and frequency
     - Espressif CPU cycle CSR, 160 MHz
     - ``mcycle``, approximately 1 GHz
   * - Monitor clock
     - SYSTIMER MMIO, 16 MHz
     - CLINT ``mtime`` MMIO, 4 MHz
   * - Hardware runs
     - Nine off/on/profile runs, October 26, 2026
     - Off/on, October 22, 2026

CPU counters measure the benchmark; the system timer measures monitor intervals.
ESP32-C3 calibration over approximately 10 ms gives a CPU/timer ratio near 10.
Three S7 calibrations per run give 999.98--1000.00 MHz against the configured
4 MHz CLINT timebase. U74 harts are not used in the S7 benchmark.

ESP32-C3 needs two evaluation-only adaptations: declaring lock-free 32-bit timer
access for the UP, PM-disabled configuration, and placing monitor functions in
IRAM. All ESP32-C3 builds use the same timer-capability declaration. The timer
accessor is unchanged except in the explicitly labeled timer-only variant.
The Espressif HAL revision is
``aa20ec213be90548a9318d3cd678bb608ce427af``. These results do not establish
upstream ESP32 support. S7 uses a local DDR/UART overlay matching the SPL:
the payload starts at ``0x40000000``, UART input clock is 24 MHz, and the
CLINT timebase is 4 MHz.

Method
******

* Preload the monitor's IRQ and hold records with long intervals,
  outside the timed region, so the short test operations use the ordinary path.
* Warm up each operation for 4096 iterations, then measure 31 batches of 4096
  iterations. Each operation is called through the same non-inlined wrapper
  and indirect-call loop. No console output or statistics reads occur within
  a timed batch.
* Lock scheduling during the benchmark. ESP32-C3 also masks interrupt sources
  through its interrupt controller, leaving the CPU's global IRQ-enable bit set
  between operations. Each public lock remains an outermost lock. S7 leaves
  interrupt sources enabled.
* Use compiler barriers around the external CPU-counter samples. The S7 samples
  also use hardware fences. Their boundary cost is amortized across the batch.
* Divide elapsed CPU cycles by 4096 and take the median of the batch averages.
  ESP32-C3 has two runs per off/on/profile state with the optimized timer and
  one per state with the original timer. S7 has one run per state. Subtract
  each state's empty-wrapper median, then subtract the off cost from the on
  cost. Do not pool different timer configurations or profile/production builds.
* All nine ESP32-C3 runs and both S7 runs complete without operation errors or
  tracking overflows. ESP32-C3 checks that its preloaded maxima do not change
  during each case. These statistics are not single-call percentiles.

ESP32-C3 component estimates
============================

The local profiling copy measures each internal timer call. It uses CPU-counter
reads with compiler memory barriers. Back-to-back reads give a median probe cost
of one CPU cycle; subtract this cost from every profiled span. Counter
accumulation occurs after each span, outside its measured boundaries.

The two rows under Extra mean:

* **Timer reads:** estimated cost of all internal timer calls in the operation.
  An IRQ or spinlock pair takes two samples; the semaphore, mutex and message
  queue pairs take four samples each.
* **Other monitor work:** Extra minus Timer reads. This combines entry and exit
  work, including interval setup and cleanup, lock matching, maximum comparisons,
  hook calls and conditions. It is a residual, not a separate direct measurement.
  The ordinary path does not replace a maximum record.

The estimates explain cost, not exact instruction-level attribution. Probes
can change register allocation and code layout even after their read cost is
subtracted. The residual also absorbs those differences. Use Off, On and Extra
from the unmodified builds for total overhead, not the slower profiling builds.
These rows do not measure the monitor's readout error or exact IRQ-off edges.

Ordinary-path results
*********************

Each operation is one complete pair. Spinlocks and mutexes are uncontended;
semaphore and message-queue operations are nonblocking. A UP spinlock does not
exercise inter-CPU atomic acquisition or waiting. Counts are CPU cycles, not
system-timer ticks. ESP32-C3 cells show **elapsed us (CPU cycles)**. Round CPU
counts to whole cycles, then divide by 160 to obtain us. At 160 MHz, one CPU
cycle is 6.25 ns. The monitor's 16 MHz SYSTIMER tick is 62.5 ns, or ten CPU
cycles; it is not the unit used to measure CPU overhead. S7's separate cycle
and time columns use unrounded differences and its nominal 1 GHz frequency.
Times are converted from CPU counts, not measured with an external clock.

ESP32-C3 with a timer-only optimization
=======================================

This separate local timer patch gives ``sys_clock_cycle_get_32()`` a dedicated
accessor: request a SYSTIMER snapshot, wait for its valid flag, then read only
the low word. The 64-bit accessor and the 16 MHz timebase are unchanged. Both
off and on builds use this same patch and the unmodified monitor implementation;
there is no CPU-clock substitution or monitor fast-path change. The patch has
not been included in the review branch.

Six hardware runs on October 26, 2026 provide two runs each with monitoring
off, on and profiled. The per-operation total-cost medians agree between
repeated boots to within one CPU cycle.

.. list-table:: ESP32-C3, optimized 32-bit timer accessor, 160 MHz
   :header-rows: 1
   :stub-columns: 1
   :widths: 30 14 14 14 14 14

   * - Elapsed us (CPU cycles)
     - IRQ lock/unlock
     - Spin lock/unlock
     - Semaphore give/take
     - Mutex lock/unlock
     - Message queue put/get
   * - Off
     - 0.056 (9)
     - 0.056 (9)
     - 0.456 (73)
     - 1.044 (167)
     - 1.363 (218)
   * - On
     - 1.425 (228)
     - 1.738 (278)
     - 3.800 (608)
     - 4.300 (688)
     - 4.738 (758)
   * - Extra total
     - 1.369 (219)
     - 1.681 (269)
     - 3.344 (535)
     - 3.256 (521)
     - 3.375 (540)
   * - Extra: timer reads (estimate)
     - 0.838 (134)
     - 0.831 (133)
     - 1.681 (269)
     - 1.675 (268)
     - 1.675 (268)
   * - Extra: other monitor work (residual)
     - 0.531 (85)
     - 0.850 (136)
     - 1.663 (266)
     - 1.581 (253)
     - 1.700 (272)

Original timers
===============

The following tables use the original timer accessors from the measured revision.

.. list-table:: ESP32-C3, 160 MHz
   :header-rows: 1
   :stub-columns: 1
   :widths: 30 14 14 14 14 14

   * - Elapsed us (CPU cycles)
     - IRQ lock/unlock
     - Spin lock/unlock
     - Semaphore give/take
     - Mutex lock/unlock
     - Message queue put/get
   * - Off
     - 0.056 (9)
     - 0.056 (9)
     - 0.456 (73)
     - 1.044 (167)
     - 1.363 (218)
   * - On
     - 1.988 (318)
     - 2.313 (370)
     - 4.913 (786)
     - 5.438 (870)
     - 5.863 (938)
   * - Extra total
     - 1.931 (309)
     - 2.256 (361)
     - 4.456 (713)
     - 4.394 (703)
     - 4.500 (720)
   * - Extra: timer reads (estimate)
     - 1.388 (222)
     - 1.394 (223)
     - 2.781 (445)
     - 2.788 (446)
     - 2.800 (448)
   * - Extra: other monitor work (residual)
     - 0.544 (87)
     - 0.863 (138)
     - 1.675 (268)
     - 1.606 (257)
     - 1.700 (272)

.. list-table:: JH7110 S7, approximately 1 GHz
   :header-rows: 1
   :widths: 40 14 14 16 16

   * - Operation
     - Off (cycles)
     - On (cycles)
     - Extra cycles
     - Extra time (us)
   * - ``irq_lock()`` / ``irq_unlock()``
     - 18
     - 262
     - 244
     - 0.244
   * - ``k_spin_lock()`` / ``k_spin_unlock()``
     - 18
     - 344
     - 326
     - 0.326
   * - Semaphore give / take
     - 160
     - 806
     - 646
     - 0.646
   * - Mutex lock / unlock
     - 406
     - 1012
     - 606
     - 0.606
   * - Message queue put / get
     - 444
     - 1082
     - 638
     - 0.638

The ESP32-C3 empty-wrapper median is approximately 12 CPU cycles in all states;
both S7 values are approximately 18 cycles. After this subtraction, S7's
uninstrumented architecture IRQ control costs 18/26 cycles (off/on).
S7 also shows an 8-cycle shift for the CPU-counter control and an 18-cycle shift
for the timer-read control.
The architecture IRQ control has identical instructions and address in both S7
images. The cause of these shifts has not been isolated; do not attribute every
cycle of an enabled/disabled difference to monitor instructions alone.
S7 has only one run per state; alternating reruns are needed to assess
run-to-run variation.

Timer-read cost
===============

The separate ``k_cycle_get_32()`` control measures the timer accessor in the
enabled builds, after empty-wrapper subtraction. On ESP32-C3, one CPU cycle
is 6.25 ns at 160 MHz. The counts below are CPU cycles, not ticks of the
16 MHz SYSTIMER used by the monitor.

.. list-table:: ESP32-C3 standalone timer-read control
   :header-rows: 1
   :widths: 55 45

   * - Timer configuration
     - One read, us (CPU cycles)
   * - Original timer
     - 0.750 (120)
   * - Optimized 32-bit accessor
     - 0.475 (76)

This standalone control includes the accessor call and storing its result.
Twice the optimized control gives 0.950 us, but directly profiling the monitor's
two internal calls estimates 0.831--0.838 us. Their call paths and surrounding
code differ. Use the internal estimates in the overhead breakdown rather than
treating twice the standalone control as an exact component measurement.
The timer-only change saves about 0.55 us per IRQ or spinlock pair without
changing the production monitor implementation.
On S7, the same single-read control costs approximately 46 CPU cycles
(0.046 us), or an estimated 92 CPU cycles (0.092 us) for two reads.

All reported lock-pair overheads include their clock samples. The clock-read
time is not the same as the timer's tick period or resolution.

Footprint
*********

Footprint uses a separate, identical-source minimal application containing one
IRQ pair, one spinlock pair and one ``printk()`` call. It does not call the
monitor snapshot/reset functions or contain benchmark calibration and validation.
The timing application's extra self-check code is therefore not counted here.
These figures come from final ELF sections and linker layouts; the S7 footprint
variants were compiled only, not run on the board. Both platforms use their
original timers for this table; it does not include the timer optimization's
footprint.

.. list-table:: Enabled-minus-disabled footprint, bytes
   :header-rows: 1
   :widths: 40 30 30

   * - Item
     - ESP32-C3
     - JH7110 S7
   * - Initialized image payload
     - 1964
     - 1752
   * - Static data
     - 180
     - 288
   * - Code/image payload also resident in RAM
     - 1792 (IRAM code)
     - 1752 (complete loaded payload)
   * - Total reserved RAM, including RAM-resident code and alignment
     - 1968 (shared SRAM)
     - 2032 (DDR)

Image payload sums allocated ``PROGBITS`` sections, excluding ESP32 dummy
alias/alignment sections and ELF debug information. Static data includes
initialized writable data and zero/uninitialized storage. Total RAM measures
the reserved address span, counting aliased IRAM/DRAM SRAM only once on ESP32-C3.
Padding can absorb growth: the S7 total reuses eight bytes of boundary padding,
so it is not simply the sum of its payload and static-data increments.
ESP32-C3's aligned binary file length is unchanged despite the payload growth.

The per-CPU monitor state is 152 bytes on ESP32-C3 and 272 bytes on S7, plus a
one-byte ready flag. These are not complete RAM totals. Lock sizes and alignment
also matter: on ESP32-C3, enabling the monitor selects nonzero lock sizes; S7
forces ``CONFIG_NONZERO_SPINLOCK_SIZE=y`` in both builds to keep lock layouts
equal. Reserved stack and heap sizes do not change; peak stack use was not
measured. Shell costs are not included.

Interpretation
**************

Instrumentation is significant relative to an otherwise empty critical section.
Its workload cost depends on how often the application enters monitored paths.
For example, 10,000 isolated empty IRQ pairs per second would consume about
1.37% of one ESP32-C3 CPU with the optimized timer, 1.93% with the original timer,
or 0.244% of one S7 CPU, in additional CPU time.
This is a rate-based estimate, not a measured application slowdown.

All configurations are UP-only and use warm, short workloads. They do not establish
SMP contention cost, cold-cache behavior, application throughput, worst-case
interrupt latency, or the cost of replacing a maximum record. Re-measure the
target workload before using these figures for a latency budget.

Source data
***********

:download:`Download ESP32-C3 breakdown batch counts <critical_section_monitor_data/breakdown.csv>`
contains the nine October 26 runs, in this order: optimized off/on/profile,
original off/on/profile, then optimized profile/on/off. Each row has 31 raw
CPU-cycle batch counts. The ``total`` metric covers the complete operation;
``timer_begin`` and ``timer_end`` are timer-call spans from profiling builds
only. ``post`` retains diagnostic exit-processing spans, which are not needed
for the combined residual row. ``events_per_operation`` gives the call count
per operation.

For a profiled span, divide its batch count by ``iterations``, then subtract
``probe_gap_cpu_cycles * events_per_operation``. Sum corrected begin/end timer
costs per batch before taking the median. Pool only matching configurations.
Calculate other monitor work as Extra minus the timer estimate. The file
contains 126 rows covering 1953 complete-operation batches and their profiles.
Off/on totals never include probe-build timings. The local test harness and
profiling probes are not part of the monitor implementation.

:download:`Download ordinary-path batch counts <critical_section_monitor_data/steady.csv>`
contains the earlier ESP32-C3 runs from October 20 and both S7 runs, including
nested locks. Each row contains 31 elapsed CPU-cycle counts, one per batch;
``iterations`` is the number of operations per batch. ESP32-C3 runs 1 and 4
have monitoring off; runs 2 and 3 have it on. S7 run 1 is off and run 2 is on.
Divide each batch count by ``iterations``, group by platform/monitor/operation,
and take the median to reproduce the raw costs. Empty-wrapper subtraction
reproduces the S7 table and the earlier ESP32-C3 results.

:download:`Download optimized-timer batch counts <critical_section_monitor_data/steady-timer32.csv>`
contains the three earlier ESP32-C3 runs with the timer-only patch. Run 1
has monitoring off; runs 2 and 3 have it on. The format and calculation are
the same. Analyze each CSV separately so the two timer configurations are not
mixed.

The earlier ESP32-C3 data used a different harness with interrupt sources
enabled. It is retained for comparison, not pooled into the October 26 tables.
