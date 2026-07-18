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
