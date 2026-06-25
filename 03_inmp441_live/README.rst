INMP441 text monitor
====================

Captures audio from an INMP441 I2S microphone and prints simple text
stats over serial every 100 ms:

::

  rms=1234 peak=4200 zcr=42 est_hz=840

Fields:

- ``rms`` - loudness (root-mean-square amplitude)
- ``peak`` - loudest sample in the chunk
- ``zcr`` - zero-crossing count (rough pitch activity indicator)
- ``est_hz`` - very rough frequency estimate from zero crossings

Build and flash for ``nrf54lm20dk/nrf54lm20a/cpuapp`` (or ``nrf54lm20b``).

View on laptop
--------------

Option A: nRF Connect serial terminal at **115200 baud**

Option B: Python monitor::

  pip3 install pyserial
  python3 tools/monitor.py --port /dev/cu.usbmodemXXXX

If unsure which port to use::

  python3 tools/listen.py

Wiring
------

+------------------+-------------+------------------+
| INMP441 signal   | DK pin      | GPIO / TDM role  |
+==================+=============+==================+
| SCK              | 14          | P3.03 / SCK_M    |
| WS               | 8           | P1.13 / FSYNC_M  |
| SD               | 18          | P3.06 / SDIN     |
| VDD              | 3V3         |                  |
| GND              | GND         |                  |
| L/R              | GND         | left channel     |
+------------------+-------------+------------------+
