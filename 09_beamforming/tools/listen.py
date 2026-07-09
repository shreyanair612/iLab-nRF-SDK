#!/usr/bin/env python3
"""Quick serial diagnostic for the INMP441 firmware."""

from __future__ import annotations

import glob
import sys
import time

import serial

PORTS = sorted(glob.glob("/dev/cu.usbmodem*"))
BAUDS = (115200, 921600)


def sniff(port: str, baud: int, seconds: float = 3.0) -> None:
    print(f"\n=== {port} @ {baud} ===")
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"open failed: {exc}")
        return

    print("Press RESET on the DK now...")
    time.sleep(0.5)
    ser.reset_input_buffer()

    data = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            data.extend(chunk)

    ser.close()

    if not data:
        print("no data")
        return

    print(f"got {len(data)} bytes")
    text = bytes(data).decode("utf-8", errors="replace")
    clean = "".join(ch if ch.isprintable() or ch in "\r\n\t" else "." for ch in text)
    if clean.strip():
        print("text:")
        print(clean[:800])

    magic = bytes(data).count(0x5A)
    print(f"0x5A bytes (possible frame markers): {magic}")
    print(f"first 32 bytes hex: {bytes(data[:32]).hex(' ')}")


def main() -> int:
    if not PORTS:
        print("No /dev/cu.usbmodem* ports found.")
        return 1

    for port in PORTS:
        for baud in BAUDS:
            sniff(port, baud)

    print("\nLook for:")
    print("- text mentioning INMP441 / Streaming PCM / errors")
    print("- non-zero 0x5A count on the port that receives binary frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
