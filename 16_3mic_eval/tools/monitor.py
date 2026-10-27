#!/usr/bin/env python3
"""
Plain serial console for the device's USB port (text mode).

    python3 tools/monitor.py                  # finds the port by itself
    python3 tools/monitor.py --port /dev/cu.usbmodemXXXX

Type commands (help, levels on, start, stop ...) and press Enter. Ctrl+C exits.
"""
from __future__ import annotations

import argparse
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device_link  # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Install it with:  pip3 install pyserial")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    args = ap.parse_args()
    port = args.port or device_link.pick_port()
    s = serial.Serial(port, 115200, timeout=0.1)
    s.dtr = True
    s.write(b"\nbinary_mode off\n")
    print(f"connected to {port}. Type 'help'. Ctrl+C to quit.")

    def reader():
        while True:
            try:
                data = s.read(4096)
            except (OSError, serial.SerialException):
                return
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            s.write(line.strip().encode() + b"\n")
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


if __name__ == "__main__":
    main()
