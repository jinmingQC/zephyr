.. zephyr:board:: rpi_3b

Overview
********

The Raspberry Pi 3 Model B board contains a Broadcom BCM2837 system-on-chip
with four Arm Cortex-A53 cores. This port boots Zephyr in AArch64 EL1 and
supports the architectural timer, a PL011 serial console, and SMP using the
BCM2836 local mailbox interrupts.

The initial port is intentionally focused on low-level SMP and debug work. It
does not provide an SD host driver.

Hardware
********

* Broadcom BCM2837
* Four Arm Cortex-A53 cores
* 1 GiB RAM (the default Zephyr image uses an 8 MiB region)
* PL011 UART on GPIO 14/15 when Bluetooth UART is disabled

Building
********

Build the hello world sample with:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: rpi_3b
   :goals: build
   :compact:

Booting
*******

Use a FAT-formatted Raspberry Pi boot partition containing the normal
Raspberry Pi 3 firmware files and copy ``build/zephyr/zephyr.bin`` to its root.
Add the following to ``config.txt``:

.. code-block:: cfg

   arm_64bit=1
   kernel=zephyr.bin
   enable_uart=1
   dtoverlay=disable-bt

The firmware loads the 64-bit ``zephyr.bin`` into RAM at ``0x200000``. Zephyr does not
access the SD card after control is transferred.

Serial Console
==============

Connect a 3.3 V USB-to-TTL serial adapter to the 40-pin header:

* GPIO 14 / pin 8: board TX
* GPIO 15 / pin 10: board RX
* GND / pin 6: ground

Use 115200 baud, 8 data bits, no parity, and one stop bit.

Limitations
***********

* Only the PL011 serial console, architectural timer, local interrupt
  controller, legacy interrupt demultiplexing, and SMP mailbox IPIs are in the
  initial scope.
* Runtime SD, USB, networking, Bluetooth, and VideoCore mailbox services are
  not enabled.
* The port targets Raspberry Pi 3 Model B revision 1.2 and is expected to also
  work on BCM2837B0 variants after board-specific firmware testing.

IPI idle wake benchmark
***********************

The idle wake benchmark from ``tests/benchmarks/ipi_metric`` was run on a
Raspberry Pi 3 Model B revision 1.2 with all four Cortex-A53 CPUs online.  Each
measurement window is five seconds long.  The comparison uses twelve windows
from each image, covering 5 through 60 seconds of execution.

The two images used identical benchmark and SMP configurations except for the
idle CPU optimization:

* Baseline: ``CONFIG_IPI_OPTIMIZE=y`` and ``CONFIG_IPI_OPTIMIZE_IDLE=n``
* Optimized: ``CONFIG_IPI_OPTIMIZE=y`` and ``CONFIG_IPI_OPTIMIZE_IDLE=y``

The summary values are arithmetic means across the twelve windows.  IPI per
wake is calculated from the aggregate IPI and wake counts.

.. list-table:: Benchmark summary
   :header-rows: 1

   * - Metric
     - Baseline
     - Optimized
     - Change
   * - Wake round trips per window
     - 1,755,873.42
     - 2,583,449.58
     - +47.13%
   * - IPI count per window
     - 6,556,908.00
     - 2,583,450.58
     - -60.60%
   * - IPI count per wake round trip
     - 3.7343
     - 1.0000
     - -73.22%
   * - Average wake latency, cycles
     - 22.00
     - 15.00
     - -31.82%
   * - Mean maximum wake latency, cycles
     - 52.50
     - 26.42
     - -49.68%

With idle CPU optimization enabled, the benchmark issues approximately one IPI
per completed wake round trip instead of 3.73.  It completes 47.13% more wake
round trips while issuing 60.60% fewer IPIs and reducing average wake latency
from 22 to 15 cycles.

Baseline measurements
=====================

.. list-table::
   :header-rows: 1

   * - Elapsed, ms
     - Wake round trips
     - IPI count
     - Average latency, cycles
     - Maximum latency, cycles
   * - 5,000
     - 1,757,495
     - 6,550,842
     - 22
     - 53
   * - 10,000
     - 1,757,649
     - 6,554,250
     - 22
     - 53
   * - 15,000
     - 1,755,509
     - 6,555,771
     - 22
     - 53
   * - 20,000
     - 1,756,858
     - 6,555,681
     - 22
     - 52
   * - 25,000
     - 1,754,530
     - 6,562,296
     - 22
     - 52
   * - 30,000
     - 1,755,528
     - 6,555,657
     - 22
     - 52
   * - 35,000
     - 1,757,740
     - 6,554,169
     - 22
     - 52
   * - 40,000
     - 1,753,127
     - 6,564,102
     - 22
     - 52
   * - 45,000
     - 1,755,997
     - 6,555,078
     - 22
     - 53
   * - 50,000
     - 1,753,860
     - 6,563,262
     - 22
     - 53
   * - 55,000
     - 1,756,961
     - 6,555,681
     - 22
     - 53
   * - 60,000
     - 1,755,227
     - 6,556,107
     - 22
     - 52

Optimized measurements
======================

.. list-table::
   :header-rows: 1

   * - Elapsed, ms
     - Wake round trips
     - IPI count
     - Average latency, cycles
     - Maximum latency, cycles
   * - 5,000
     - 2,572,120
     - 2,572,121
     - 15
     - 38
   * - 10,000
     - 2,589,621
     - 2,589,622
     - 15
     - 25
   * - 15,000
     - 2,565,755
     - 2,565,756
     - 15
     - 26
   * - 20,000
     - 2,565,489
     - 2,565,490
     - 15
     - 25
   * - 25,000
     - 2,602,510
     - 2,602,511
     - 15
     - 25
   * - 30,000
     - 2,602,498
     - 2,602,499
     - 15
     - 25
   * - 35,000
     - 2,565,782
     - 2,565,783
     - 15
     - 26
   * - 40,000
     - 2,565,768
     - 2,565,769
     - 15
     - 25
   * - 45,000
     - 2,602,257
     - 2,602,258
     - 15
     - 26
   * - 50,000
     - 2,602,284
     - 2,602,285
     - 15
     - 26
   * - 55,000
     - 2,565,716
     - 2,565,717
     - 15
     - 25
   * - 60,000
     - 2,601,595
     - 2,601,596
     - 15
     - 25
