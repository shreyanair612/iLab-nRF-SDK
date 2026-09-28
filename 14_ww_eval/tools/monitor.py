#!/usr/bin/env python3
"""
Serial console for the test rig.

    python3 tools/monitor.py                 # finds the DK's port by itself
    python3 tools/monitor.py --port /dev/cu.usbmodem0010...  --baud 1000000

Type device commands (help, levels, set_preset bf_lr, start, stop ...) and
press Enter. Every line, sent and received, is also written to
sessions/<date>_<time>.txt so nothing is lost.

After each run the device prints a summary; this tool pulls out the one number
you need for the test log and prints it on its own line.

Ctrl+C or typing `quit` exits.
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is missing. Install it with:  pip3 install pyserial")

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSIONS = os.path.join(PROJECT, "sessions")
BAUD = 1000000

BOLD = "\033[1m"
RED = "\033[31m"
GOLD = "\033[33m"
DIM = "\033[2m"
RESET = "\033[0m"
if not sys.stdout.isatty():
    BOLD = RED = GOLD = DIM = RESET = ""


def candidate_ports() -> list[str]:
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyACM*"))
    return ports


def probe(port: str, baud: int) -> bool:
    """True if the evaluation firmware answers 'hello' on this port."""
    try:
        with serial.Serial(port, baud, timeout=0.1) as s:
            s.reset_input_buffer()
            s.write(b"\nbinary_mode off\nhello\n")
            deadline = time.time() + 1.5
            buf = b""
            while time.time() < deadline:
                buf += s.read(512)
                if b"ok: hello" in buf or b"ok: binary_mode off" in buf:
                    return True
    except (OSError, serial.SerialException):
        return False
    return False


def pick_port(baud: int) -> str:
    ports = candidate_ports()
    if not ports:
        sys.exit("No USB serial port found. Plug the DK's IMCU USB port into this Mac and switch it on.")
    for p in ports:
        if probe(p, baud):
            return p
    names = "\n  ".join(ports)
    sys.exit(
        "Found USB serial ports but none answered:\n  " + names + "\n"
        "Check the firmware is flashed (tools/rig.sh flash), press the DK's RESET button, and try again.\n"
        "If the text looks garbled when you pass --port, the adapter may not support 1 Mbaud;\n"
        "see 'Link speed' in RIG_SETUP.md.")


class Summary:
    """Picks the scored path's result out of the device's end-of-run summary."""

    def __init__(self):
        self.active = False
        self.scored = None
        self.angle = None
        self.dist = None
        self.env = None
        self.quality = None
        self.det = None

    def feed(self, line: str):
        if "Wake-Word Beamforming Evaluation" in line:
            self.__init__()
            self.active = True
            return None
        if not self.active:
            return None
        m = re.search(r"Scored path:\s+(\S+)", line)
        if m:
            self.scored = m.group(1)
        m = re.search(r"Angle / distance:\s+(-?\d+) degrees / (\d+) cm", line)
        if m:
            self.angle, self.dist = m.group(1), m.group(2)
        m = re.search(r"Environment:\s+(\S+)", line)
        if m:
            self.env = m.group(1)
        if self.scored and line.startswith(self.scored + " ") and " yes " in line:
            parts = line.split()
            if len(parts) >= 4 and parts[3].isdigit():
                self.det = int(parts[3])
        m = re.search(r"Result quality:\s+(.+)", line)
        if m:
            self.quality = m.group(1).strip()
            self.active = False
            return self
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=BAUD)
    args = ap.parse_args()

    port = args.port or pick_port(args.baud)
    os.makedirs(SESSIONS, exist_ok=True)
    log_path = os.path.join(SESSIONS, time.strftime("%Y-%m-%d_%H%M%S") + ".txt")
    log = open(log_path, "a", encoding="utf-8")

    def write_log(prefix: str, text: str):
        log.write(f"{time.strftime('%H:%M:%S')} {prefix}{text}\n")
        log.flush()

    ser = serial.Serial(port, args.baud, timeout=0.1)
    ser.write(b"\nbinary_mode off\n")
    print(f"{BOLD}Connected to {port} at {args.baud} baud.{RESET} Logging to {os.path.relpath(log_path, PROJECT)}")
    print(f"{DIM}Type 'help' for commands, 'levels on' to watch the microphones, 'quit' to exit.{RESET}")

    stop = threading.Event()
    summary = Summary()

    def reader():
        buf = b""
        while not stop.is_set():
            try:
                chunk = ser.read(512)
            except (OSError, serial.SerialException) as e:
                print(f"\n{RED}Lost the serial connection: {e}{RESET}")
                stop.set()
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if not line:
                    continue
                write_log("< ", line)
                color = ""
                if line.startswith(">>> wake word"):
                    color = GOLD + BOLD
                elif line.startswith("err:") or "<err>" in line:
                    color = RED
                print(f"{color}{line}{RESET}" if color else line)
                done = summary.feed(line)
                if done and done.det is not None:
                    where = f"at {done.angle}° / {done.dist} cm" + (f" / {done.env}" if done.env else "")
                    msg = (f"RESULT  {done.scored}  {where}  ->  "
                           f"{done.det} detections   ({done.quality})")
                    print(f"\n{BOLD}{GOLD}{msg}{RESET}\n")
                    write_log("= ", msg)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        while not stop.is_set():
            try:
                line = input()
            except EOFError:
                break
            if line.strip() in ("quit", "exit"):
                break
            write_log("> ", line)
            ser.write((line + "\n").encode("utf-8"))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.15)
        ser.close()
        log.close()
        print(f"\nSession saved to {os.path.relpath(log_path, PROJECT)}")


if __name__ == "__main__":
    main()
