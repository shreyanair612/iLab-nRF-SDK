#!/usr/bin/env python3
"""
Read INMP441 PCM frames from the nRF54LM20 DK J-Link virtual COM port
and plot a live waveform.

Install:
  pip3 install pyserial matplotlib numpy

Usage:
  python3 tools/live_plot.py --port /dev/cu.usbmodemXXXX
  python3 tools/live_plot.py --probe
"""

from __future__ import annotations

import argparse
import glob
import struct
import sys
import time

import matplotlib.pyplot as plt
import numpy as np
import serial


FRAME_MAGIC = 0xA55A
HEADER_FMT = "<HH"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
DEFAULT_BAUD = 115200
BAUD_CANDIDATES = (115200, 921600)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Live INMP441 waveform viewer")
    parser.add_argument("--port", help="Serial port path")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="UART baud rate")
    parser.add_argument("--window-seconds", type=float, default=1.0, help="Plot window length")
    parser.add_argument("--sample-rate", type=int, default=8000, help="Expected sample rate")
    parser.add_argument(
        "--probe",
        action="store_true",
        help="Scan common macOS J-Link ports and print anything received",
    )
    return parser.parse_args()


def list_candidate_ports() -> list[str]:
    ports: list[str] = []
    for pattern in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*"):
        ports.extend(sorted(glob.glob(pattern)))
    # Prefer cu.* on macOS; drop duplicate tty.* twins when possible.
    cu_ports = [p for p in ports if "/cu." in p]
    return cu_ports or ports


def probe_port(port: str, baud: int, seconds: float = 2.0) -> None:
    print(f"\n=== {port} @ {baud} ===")
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"Could not open: {exc}")
        return

    time.sleep(0.3)
    deadline = time.time() + seconds
    collected = bytearray()

    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            collected.extend(chunk)

    ser.close()

    if not collected:
        print("No data received.")
        return

    print(f"Received {len(collected)} bytes.")
    text = bytes(collected).decode("utf-8", errors="replace")
    printable = "".join(ch if ch.isprintable() or ch in "\r\n\t" else "." for ch in text)
    if printable.strip():
        print("Text:")
        print(printable[:500])

    magic_hits = bytes(collected).count(FRAME_MAGIC & 0xFF)
    print(f"Possible frame markers (0x5A bytes): {magic_hits}")


def read_frame(ser: serial.Serial, timeout_s: float = 5.0) -> np.ndarray:
    deadline = time.time() + timeout_s

    while time.time() < deadline:
        byte = ser.read(1)
        if not byte:
            continue

        if byte[0] != (FRAME_MAGIC & 0xFF):
            continue

        rest = ser.read(HEADER_SIZE - 1)
        if len(rest) != HEADER_SIZE - 1:
            continue

        magic, count = struct.unpack(HEADER_FMT, byte + rest)
        if magic != FRAME_MAGIC or count == 0 or count > 4096:
            continue

        payload = ser.read(count * 2)
        if len(payload) != count * 2:
            continue

        return np.frombuffer(payload, dtype="<i2")

    raise TimeoutError(
        "No audio frames received. Try --probe, the other usbmodem port, "
        "or reflash after rebuilding firmware."
    )


def open_serial(port: str | None, baud: int) -> serial.Serial:
    ports = [port] if port else list_candidate_ports()
    if not ports:
        raise SystemExit("No /dev/cu.usbmodem* ports found. Is the DK plugged in?")

    last_error: Exception | None = None
    for candidate in ports:
        for candidate_baud in (baud,) if baud else BAUD_CANDIDATES:
            try:
                ser = serial.Serial(candidate, candidate_baud, timeout=0.2)
                print(f"Opened {candidate} @ {candidate_baud}")
                return ser
            except serial.SerialException as exc:
                last_error = exc

    raise SystemExit(f"Could not open serial port: {last_error}")


def main() -> int:
    args = parse_args()

    if args.probe:
        ports = list_candidate_ports()
        if not ports:
            print("No usbmodem ports found.")
            return 1
        for port in ports:
            for baud in BAUD_CANDIDATES:
                probe_port(port, baud)
        return 0

    ser = open_serial(args.port, args.baud)
    print("Press RESET on the DK if the plot stays empty.")
    time.sleep(1.0)
    ser.reset_input_buffer()

    window_samples = int(args.window_seconds * args.sample_rate)
    buffer = np.zeros(window_samples, dtype=np.int16)

    fig, ax = plt.subplots()
    line, = ax.plot(buffer)
    ax.set_ylim(-32768, 32767)
    ax.set_xlim(0, window_samples)
    ax.set_title("INMP441 live audio")
    ax.set_xlabel("Samples")
    ax.set_ylabel("Amplitude")
    plt.tight_layout()
    plt.ion()
    plt.show()

    try:
        while plt.fignum_exists(fig.number):
            chunk = read_frame(ser)
            if len(chunk) >= window_samples:
                buffer[:] = chunk[-window_samples:]
            else:
                buffer = np.roll(buffer, -len(chunk))
                buffer[-len(chunk):] = chunk

            line.set_ydata(buffer)
            fig.canvas.draw_idle()
            fig.canvas.flush_events()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
