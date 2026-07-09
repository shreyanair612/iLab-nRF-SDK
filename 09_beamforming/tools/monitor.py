#!/usr/bin/env python3
"""Print INMP441 text stats from the DK serial port."""

from __future__ import annotations

import argparse
import glob
import sys
import time

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Read INMP441 text stats")
    parser.add_argument("--port", help="Serial port, e.g. /dev/cu.usbmodem123")
    parser.add_argument("--baud", type=int, default=115200)
    return parser.parse_args()


def pick_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("No /dev/cu.usbmodem* port found.")
    if len(ports) == 1:
        return ports[0]
    print("Multiple ports found:")
    for i, port in enumerate(ports):
        print(f"  [{i}] {port}")
    choice = input("Pick port number: ").strip()
    return ports[int(choice)]


def main() -> int:
    args = parse_args()
    port = args.port or pick_port()

    print(f"Opening {port} @ {args.baud}")
    print("Press RESET on the DK if you see nothing.\n")

    with serial.Serial(port, args.baud, timeout=1) as ser:
        while True:
            line = ser.readline()
            if not line:
                continue
            print(line.decode("utf-8", errors="replace"), end="")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        sys.exit(0)
