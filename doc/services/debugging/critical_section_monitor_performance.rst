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
pair adds about 1.38 us and an uncontended spinlock pair adds about 1.69 us.
That timer patch is not part of the measured review revision. The original
ESP32-C3 timer gives 1.94 us and 2.24 us, respectively; JH7110 S7 gives
0.244 us and 0.326 us. These are single-core microbenchmarks, not application-wide
slowdowns or worst-case latency bounds.

Setup
*****

The measured revision is ``8414a073013e8896cedc05cfa2181da202e24cc6``.
Both targets use Zephyr SDK 1.0.1, GCC 14.3.0 and size optimization (``-Os``).
SMP, assertions, spinlock validation, spinlock time-limit checking, power
management, logging and shell are disabled. The monitor tracks up to four held
spinlocks. The monitor algorithm is unchanged in every configuration. Original
timer results and the ESP32-C3 timer-only optimization are reported separately.

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
     - Off/on/on/off, October 20, 2026
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

* Preload the monitor's IRQ and hold records with approximately 1 ms intervals,
  outside the timed region, so the short test operations use the ordinary path.
* Warm up each operation for 4096 iterations, then measure 31 batches of 4096
  iterations. Each operation is called through the same non-inlined wrapper
  and indirect-call loop. No console output or statistics reads occur within
  a timed batch.
* Lock scheduling during the benchmark, but leave interrupts enabled between
  operations. Each tested lock/unlock pair manages its own IRQ state.
* Use compiler barriers around the external CPU-counter samples. The S7 samples
  also use hardware fences. Their boundary cost is amortized across the batch.
* Divide elapsed CPU cycles by 4096 and take the median of the batch averages.
  For ESP32-C3 with the original timer, pool the two runs for each monitor state
  (62 batches). The optimized-timer configuration has one off run and two on
  runs (31 and 62 batches); S7 has 31 batches per state. Subtract that state's
  empty-wrapper median, then subtract the off cost from the on cost. Do not
  pool results from different timer configurations.
* All nine runs complete with zero operation errors; enabled runs report no
  tracking overflows. These statistics are not single-call percentiles.

Ordinary-path results
*********************

Each row is one complete operation pair. Spinlocks and mutexes are uncontended;
semaphore and message-queue operations are nonblocking. A UP spinlock does not
exercise inter-CPU atomic acquisition or waiting. Counts are CPU cycles, not
system-timer ticks. Cycle columns are rounded to the nearest whole cycle; times
use unrounded differences and nominal 160 MHz / 1 GHz frequencies.

ESP32-C3 with a timer-only optimization
=======================================

This separate local timer patch gives ``sys_clock_cycle_get_32()`` a dedicated
accessor: request a SYSTIMER snapshot, wait for its valid flag, then read only
the low word. The 64-bit accessor and the 16 MHz timebase are unchanged. Both
off and on builds use this same patch and the unmodified monitor implementation;
there is no CPU-clock substitution or monitor fast-path change. The patch has
not been included in the review branch.

Three hardware runs on October 20, 2026 provide one off run and two on runs.
The second on run repeats the same configuration after intervening experiments;
its per-operation medians agree with the first to within one CPU cycle.

.. list-table:: ESP32-C3, optimized 32-bit timer accessor, 160 MHz
   :header-rows: 1
   :widths: 40 14 14 16 16

   * - Operation
     - Off (cycles)
     - On (cycles)
     - Extra cycles
     - Extra time (us)
   * - ``irq_lock()`` / ``irq_unlock()``
     - 9
     - 230
     - 221
     - 1.381
   * - ``k_spin_lock()`` / ``k_spin_unlock()``
     - 10
     - 280
     - 270
     - 1.688
   * - Semaphore give / take
     - 73
     - 606
     - 533
     - 3.331
   * - Mutex lock / unlock
     - 166
     - 688
     - 522
     - 3.262
   * - Message queue put / get
     - 211
     - 752
     - 541
     - 3.381

Original timers
===============

The following tables use the original timer accessors from the measured revision.

.. list-table:: ESP32-C3, 160 MHz
   :header-rows: 1
   :widths: 40 14 14 16 16

   * - Operation
     - Off (cycles)
     - On (cycles)
     - Extra cycles
     - Extra time (us)
   * - ``irq_lock()`` / ``irq_unlock()``
     - 9
     - 320
     - 311
     - 1.944
   * - ``k_spin_lock()`` / ``k_spin_unlock()``
     - 10
     - 368
     - 358
     - 2.238
   * - Semaphore give / take
     - 73
     - 788
     - 715
     - 4.469
   * - Mutex lock / unlock
     - 166
     - 866
     - 700
     - 4.375
   * - Message queue put / get
     - 211
     - 930
     - 719
     - 4.494

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

The empty-wrapper medians are 11 cycles with the monitor disabled and 10 enabled
on ESP32-C3; both S7 values are approximately 18 cycles. After this subtraction,
the uninstrumented architecture IRQ control costs 9/9 cycles (off/on) on
ESP32-C3 but 18/26 on S7. S7 also shows an 8-cycle shift for
the CPU-counter control and an 18-cycle shift for the timer-read control.
The architecture IRQ control has identical instructions and address in both S7
images. The cause of these shifts has not been isolated; do not attribute every
cycle of an enabled/disabled difference to monitor instructions alone.
ESP32-C3's repeated per-run medians agree to within one CPU cycle. S7 has only
one run per state; alternating reruns are needed to assess run-to-run variation.

Timer-read cost
===============

The separate ``k_cycle_get_32()`` control measures the timer accessor in the
enabled builds, after empty-wrapper subtraction. On ESP32-C3, one CPU cycle
is 6.25 ns at 160 MHz. The counts below are CPU cycles, not ticks of the
16 MHz SYSTIMER used by the monitor.

.. list-table:: ESP32-C3 timer-read cost, approximately
   :header-rows: 1
   :widths: 28 18 18 18 18

   * - Timer configuration
     - One read (CPU cycles)
     - One read (us)
     - Two reads (CPU cycles, estimated)
     - Two reads (us, estimated)
   * - Original timer
     - 120
     - 0.750
     - 240
     - 1.500
   * - Optimized 32-bit accessor
     - 76
     - 0.475
     - 152
     - 0.950

An ordinary non-nested IRQ or spinlock pair takes two clock samples. With the
optimized timer, the estimated 0.950 us for these reads is a large part of the
total overhead: 1.381 us per IRQ pair and 1.688 us per spinlock pair.
The timer-only change saves approximately 88 CPU cycles (0.550 us) across two
reads; the measured total overhead falls by 90 cycles for IRQ pairs and
88 cycles for spinlock pairs, without changing the monitor implementation.
On S7, the same single-read control costs approximately 46 CPU cycles
(0.046 us), or an estimated 92 CPU cycles (0.092 us) for two reads.

The single-read measurements include the accessor call and storing its result,
not just the MMIO load. The two-read figures are twice that control measurement,
not an independent profile of the monitor's internal calls. All reported
lock-pair overheads include the cost of those clock samples.

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
1.38% of one ESP32-C3 CPU with the optimized timer, 1.94% with the original timer,
or 0.244% of one S7 CPU, in additional CPU time.
This is a rate-based estimate, not a measured application slowdown.

All configurations are UP-only and use warm, short workloads. They do not establish
SMP contention cost, cold-cache behavior, application throughput, worst-case
interrupt latency, or the cost of replacing a maximum record. Re-measure the
target workload before using these figures for a latency budget.

Source data
***********

:download:`Download ordinary-path batch counts <critical_section_monitor_data/steady.csv>`
contains all ordinary-path operations and controls from the six original-timer runs, including
nested locks. Each row contains 31 elapsed CPU-cycle counts, one per batch;
``iterations`` is the number of operations per batch. Run IDs follow the order
in the setup table. Divide each batch count by ``iterations``, group by
platform/monitor/operation, and take the median to reproduce the raw costs.
Apply the empty-wrapper subtraction described above to reproduce the tables.

:download:`Download optimized-timer batch counts <critical_section_monitor_data/steady-timer32.csv>`
contains the three additional ESP32-C3 runs with the timer-only patch. Run 1
has monitoring off; runs 2 and 3 have it on. The format and calculation are
the same. Analyze each CSV separately so the two timer configurations are not
mixed.
