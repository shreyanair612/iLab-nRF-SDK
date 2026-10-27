"""
Serial link to the evaluation firmware over its USB CDC ACM port.

    link = DeviceLink.open()            # finds the port by itself
    link.command("set_angle 30")        # waits for the ok:/err: reply
    rec = link.record(on_started=...)   # blocks until RUN_END, returns the audio

The device is driven in binary mode; every packet is decoded by
serial_protocol.StreamParser. Audio chunks are collected per stream by
wav_writer.StreamWriter, which fills any holes with silence and records them
as gaps so a degraded run is never mistaken for a clean one.
"""
from __future__ import annotations

import glob
import json
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is missing. Install it with:  pip3 install pyserial")

import serial_protocol as sp
from wav_writer import StreamWriter

SAMPLE_RATE = 16000


def candidate_ports() -> List[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyACM*"))


def probe(port: str) -> bool:
    """True if the evaluation firmware answers 'hello' on this port."""
    try:
        with serial.Serial(port, 115200, timeout=0.1) as s:
            s.dtr = True
            s.reset_input_buffer()
            s.write(b"\nbinary_mode off\nhello\n")
            deadline = time.time() + 1.5
            buf = b""
            while time.time() < deadline:
                buf += s.read(1024)
                if b"16_3mic_eval" in buf or b"ok: hello" in buf:
                    return True
    except (OSError, serial.SerialException):
        return False
    return False


def pick_port() -> str:
    ports = candidate_ports()
    if not ports:
        sys.exit("No USB serial port found. Plug the DK's 'nRF USB' port (not only IMCU) into this Mac "
                 "and switch the board on.")
    for p in ports:
        if probe(p):
            return p
    names = "\n  ".join(ports)
    sys.exit("Found USB serial ports but none answered as 16_3mic_eval:\n  " + names + "\n"
             "Check the firmware is flashed (tools/rig.sh flash), that BOTH USB cables are connected "
             "(IMCU for flashing, nRF USB for audio), press RESET, and try again.")


@dataclass
class Recording:
    run_id: int
    start: dict
    end: Optional[dict]
    streams: Dict[str, StreamWriter]
    stats: List[dict] = field(default_factory=list)
    log: List[str] = field(default_factory=list)
    host_started: float = 0.0
    host_ended: float = 0.0

    @property
    def complete(self) -> bool:
        return self.end is not None and all(w.complete for w in self.streams.values())

    def warnings(self) -> List[str]:
        w: List[str] = []
        if self.end is None:
            w.append("run ended without RUN_END (device disconnected?)")
        else:
            if self.end.get("frames_lost", 0):
                w.append(f"{self.end['frames_lost']} frames lost between mics and host")
            if self.end.get("capture_dropped", 0):
                w.append(f"{self.end['capture_dropped']} capture frames dropped on the device")
            if self.end.get("tx_audio_dropped", 0):
                w.append(f"{self.end['tx_audio_dropped']} audio packets dropped on the USB link")
            if self.end.get("bf_clips", 0):
                w.append(f"device beamformer clipped {self.end['bf_clips']} samples")
        for name, s in self.streams.items():
            if s.gaps:
                w.append(f"{name}: {len(s.gaps)} gap(s), {sum(g.samples for g in s.gaps)} samples filled")
        return w


class DeviceLink:
    def __init__(self, port: str, log_file=None, echo: bool = False):
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.dtr = True
        self.parser = sp.StreamParser()
        self.echo = echo
        self.log_file = log_file
        self._lock = threading.Lock()
        self._acks: List[str] = []
        self._pending: List[sp.Packet] = []
        self._text_lines: List[str] = []
        self.hello: dict = {}
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        self._enter_binary()

    @classmethod
    def open(cls, port: Optional[str] = None, **kw) -> "DeviceLink":
        return cls(port or pick_port(), **kw)

    # ------------------------------------------------------------ plumbing

    def _log(self, line: str) -> None:
        if self.echo:
            print(line)
        if self.log_file:
            self.log_file.write(time.strftime("%H:%M:%S ") + line + "\n")
            self.log_file.flush()

    def _reader(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.ser.read(4096)
            except (OSError, serial.SerialException):
                break
            if not data:
                continue
            for kind, val in self.parser.feed(data):
                if kind == "text":
                    self._on_text(val)
                elif kind == "packet":
                    self._on_packet(val)

    def _on_text(self, line: str) -> None:
        self._log("  " + line)
        with self._lock:
            self._text_lines.append(line)
            if line.startswith("ok:") or line.startswith("err:"):
                self._acks.append(line)

    def _on_packet(self, pkt: sp.Packet) -> None:
        if pkt.type in sp.TEXT_TYPES:
            line = pkt.text()
            self._log(f"  [{pkt.type_name}] {line}")
            with self._lock:
                if pkt.type in (sp.ACK, sp.ERROR):
                    self._acks.append(line)
                self._text_lines.append(line)
            return
        if pkt.type == sp.HELLO:
            try:
                self.hello = json.loads(pkt.text())
            except ValueError:
                pass
        if pkt.type != sp.AUDIO_CHUNK:
            self._log(f"  [{pkt.type_name}] {pkt.text()[:200]}")
        with self._lock:
            self._pending.append(pkt)

    def _enter_binary(self) -> None:
        self.ser.write(b"\n")
        time.sleep(0.1)
        self.command("binary_mode on", timeout=2.0)
        self.command("hello")

    def close(self) -> None:
        self._stop.set()
        try:
            self.ser.write(sp.encode_command("binary_mode off"))
            time.sleep(0.05)
        except Exception:
            pass
        self.ser.close()

    # ------------------------------------------------------------ commands

    def command(self, line: str, timeout: float = 2.0) -> str:
        """Sends one command and returns its ok:/err: reply. Raises on err or timeout."""
        with self._lock:
            self._acks.clear()
        self._log("> " + line)
        self.ser.write(sp.encode_command(line))
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if self._acks:
                    reply = self._acks.pop(0)
                    break
            time.sleep(0.01)
        else:
            raise TimeoutError(f"no reply to '{line}'")
        if reply.startswith("err:"):
            raise RuntimeError(f"'{line}' -> {reply}")
        return reply

    def drain(self) -> List[sp.Packet]:
        with self._lock:
            out = self._pending
            self._pending = []
        return out

    # ------------------------------------------------------------ recording

    def wait_for_run_start(self, timeout: Optional[float], poll: Callable[[], None] = None) -> dict:
        """Blocks until the device announces RUN_START (BTN1 or `start`)."""
        deadline = None if timeout is None else time.time() + timeout
        while deadline is None or time.time() < deadline:
            for pkt in self.drain():
                if pkt.type == sp.RUN_START:
                    return json.loads(pkt.text())
            if poll:
                poll()
            time.sleep(0.02)
        raise TimeoutError("device did not start a run")

    def record(self, start: dict, on_started: Callable[[], None] = None,
               idle_timeout: float = 10.0) -> Recording:
        """Collects audio from a run that has just started until RUN_END."""
        rec = Recording(run_id=int(start.get("run_id", 0)), start=start, end=None, streams={})
        rec.host_started = time.time()
        enabled = start.get("streams", {"left": 1, "center": 1, "right": 1, "bf": 1})
        for key, name in (("left", "raw_left"), ("center", "raw_center"), ("right", "raw_right"), ("bf", "bf_device")):
            if enabled.get(key, 1):
                rec.streams[name] = StreamWriter(name, SAMPLE_RATE, 1)
        if on_started:
            on_started()
        last = time.time()
        while True:
            pkts = self.drain()
            if pkts:
                last = time.time()
            for pkt in pkts:
                if pkt.type == sp.AUDIO_CHUNK and pkt.run_id == rec.run_id:
                    name = sp.stream_name(pkt.stream_id)
                    w = rec.streams.get(name)
                    if w is None:
                        w = rec.streams[name] = StreamWriter(name, SAMPLE_RATE, 1)
                    w.add(pkt.timestamp, pkt.payload)
                elif pkt.type == sp.AUDIO_STATS:
                    try:
                        d = json.loads(pkt.text())
                        d["sample_index"] = pkt.timestamp
                        rec.stats.append(d)
                    except ValueError:
                        pass
                elif pkt.type == sp.RUN_END:
                    rec.end = json.loads(pkt.text())
                    rec.host_ended = time.time()
                    with self._lock:
                        rec.log = list(self._text_lines)
                        self._text_lines.clear()
                    return rec
            if time.time() - last > idle_timeout:
                rec.host_ended = time.time()
                return rec
            time.sleep(0.01)
