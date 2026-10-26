.. SPDX-FileCopyrightText: Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
.. SPDX-License-Identifier: Apache-2.0

.. _critical_section_monitor_irq_spin_summary:

ESP32-C3 IRQ and spinlock summary
#################################

ESP32-C3, 160 MHz, UP, GCC 14.3.0, ``-Os``, original 16 MHz SYSTIMER.
PM, assertions, spinlock validation and shell are off. Local timer-capability
and IRAM adaptations are for evaluation only, not upstream ESP32 support.

Runtime overhead
****************

One empty, uncontended lock/unlock pair without updating a maximum record.
Units: **us (CPU cycles)**, at 160 CPU cycles/us. One system-timer tick is 62.5 ns.

.. list-table:: Ordinary-path overhead, original timer
   :header-rows: 1
   :stub-columns: 1
   :widths: 15 45 20 20

   * - Monitor
     - Breakdown
     - IRQ lock/unlock
     - Spin lock/unlock
   * - Off
     - Total
     - 0.056 (9)
     - 0.056 (9)
   * - On
     - Total
     - 1.988 (318)
     - 2.313 (370)
   * - Extra
     - Total
     - 1.931 (309)
     - 2.256 (361)
   * -
     - Two timer reads (estimate)
     - 1.388 (222)
     - 1.394 (223)
   * -
     - Other monitor work (residual)
     - 0.544 (87)
     - 0.863 (138)

Extra = On - Off. Timer reads are estimated; other work is the residual.
Totals use unmodified builds. Results are medians of warm batch averages,
not worst-case bounds.

ROM and RAM
***********

On-minus-Off for the same minimal application with one IRQ pair and one
spinlock pair; no benchmark, probes or shell.

.. list-table:: Resource increase, original timer
   :header-rows: 1
   :stub-columns: 1
   :widths: 25 50 25

   * - Resource
     - Breakdown
     - Increase
   * - ROM/image payload
     - Total
     - +1964 B
   * - RAM
     - Total reserved SRAM
     - +1968 B
   * -
     - Static data
     - +180 B
   * -
     - IRAM-resident code
     - +1792 B
   * -
     - Layout/alignment
     - -4 B

ROM counts initialized image sections, not padded binary length. RAM includes
its component rows. Stack/heap reservations are unchanged; peak stack use was
not measured.

See :ref:`critical_section_monitor_measurements` for the method and raw timing
data, and :download:`footprint counts <critical_section_monitor_data/irq-spin-footprint.csv>`.
