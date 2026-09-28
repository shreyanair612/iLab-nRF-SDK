#!/usr/bin/env python3
"""
Local dashboard and run recorder for the wake-word beamforming evaluation.

    python3 tools/dashboard_server.py --port /dev/tty.usbmodem<ID> --baud 1000000 --web-port 5000
    python3 tools/dashboard_server.py --simulate            # no hardware, synthetic device

Then open http://localhost:5000

Responsibilities
  - talk to the device over the framed serial protocol (tools/serial_protocol.py)
  - record every run into runs/<timestamp_angle_distance_label>/ (tools/run_storage.py)
  - turn streamed PCM into WAV files with honest gap accounting (tools/wav_writer.py)
  - rebuild processed paths from the raw microphones and verify them against the
    device's CRCs (tools/dsp_reference.py)
  - replay the recorded raw audio through the device once per path that was not
    scored live, so every path is scored by the same on-device model on the same
    audio (the wake-word runtime can only score one path at a time)
  - serve the browser dashboard and a small JSON API

Standard library only, plus pyserial for the real device.
"""
from __future__ import annotations

import argparse
import collections
import html
import json
import math
import os
import random
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import dsp_reference as dsp  # noqa: E402
import run_storage  # noqa: E402
import serial_protocol as sp  # noqa: E402
import wav_writer  # noqa: E402

SAMPLE_RATE = 16000
FRAME = 256
PRESETS = ["left", "right", "lr_mix", "bf_lr", "raw_3ch", "bf_3ch", "raw_vs_bf_lr",
           "raw_vs_bf_3ch", "full"]
CAPTURE_MODES = ["off", "event_window", "continuous_short", "continuous_full"]
REDUCE_MODES = ["center_only", "lr_average", "lc_average", "rc_average", "lrc_average"]
STATIC_DIR = os.path.join(HERE, "static")


def now() -> float:
    return time.time()


def jload(pkt: sp.Packet) -> Dict[str, Any]:
    try:
        return json.loads(pkt.payload.decode("utf-8", errors="replace"))
    except json.JSONDecodeError:
        return {"_raw": pkt.text()}


# ============================================================ device links

class SerialDevice:
    """Real device over pyserial. Reconnects on error."""

    def __init__(self, port: str, baud: int, on_event):
        import serial  # pyserial
        self._serial_mod = serial
        self.port = port
        self.baud = baud
        self.on_event = on_event
        self.parser = sp.StreamParser()
        self.connected = False
        self.binary = False
        self.rx_bytes = 0
        self.last_rx = 0.0
        self._ser = None
        self._wlock = threading.Lock()
        self._stop = False
        threading.Thread(target=self._reader, name="serial-reader", daemon=True).start()

    def _open(self) -> bool:
        try:
            self._ser = self._serial_mod.Serial(self.port, self.baud, timeout=0.05)
            self.connected = True
            self.binary = False
            self.parser = sp.StreamParser()
            self.on_event(("connected", None))
            time.sleep(0.3)
            self.send_line("binary_mode on")
            return True
        except Exception as e:  # noqa: BLE001
            self.connected = False
            self.on_event(("log", f"serial open failed: {e}"))
            return False

    def _reader(self) -> None:
        while not self._stop:
            if not self.connected and not self._open():
                time.sleep(2.0)
                continue
            try:
                n = self._ser.in_waiting
                data = self._ser.read(n if n else 1)
            except Exception as e:  # noqa: BLE001
                self.connected = False
                self.on_event(("disconnected", str(e)))
                try:
                    self._ser.close()
                except Exception:  # noqa: BLE001
                    pass
                continue
            if not data:
                continue
            self.rx_bytes += len(data)
            self.last_rx = now()
            for ev in self.parser.feed(data):
                self.on_event(ev)

    def send_line(self, line: str) -> None:
        payload = sp.encode_command(line) if self.binary else (line + "\n").encode()
        self.send_raw(payload)
        if line.strip() == "binary_mode on":
            self.binary = True
        elif line.strip() == "binary_mode off":
            self.binary = False

    def send_raw(self, data: bytes) -> None:
        if not self.connected or self._ser is None:
            return
        with self._wlock:
            try:
                self._ser.write(data)
            except Exception as e:  # noqa: BLE001
                self.connected = False
                self.on_event(("disconnected", str(e)))


class FakeDevice:
    """Synthetic device for exercising the dashboard without hardware.

    It speaks the same protocol as the firmware: text mode until
    'binary_mode on', then framed packets. Runs produce two synthetic
    microphone channels with speech-like bursts, periodic status/metrics,
    wake-word events on the live path, and honest CRCs. Replay passes accept
    REPLAY_AUDIO, rebuild the requested path with dsp_reference (so CRC checks
    are meaningful), stream the processed audio back, and score it.
    """

    def __init__(self, on_event):
        self.on_event = on_event
        self.connected = True
        self.binary = False
        self.rx_bytes = 0
        self.last_rx = now()
        self.parser = sp.StreamParser()
        self.run_id = 0
        self.state = "idle"
        self.cfg = self._default_cfg()
        self._lock = threading.Lock()
        self._stop_req = False
        self._replay = None
        self._rng = random.Random(7)
        self.on_event(("connected", None))
        self._emit_text("=== fake device: type 'help' ===")

    # ---- config helpers
    def _default_cfg(self):
        paths = []
        for i, n in enumerate(sp.PATH_NAMES):
            paths.append({"id": i, "name": n, "enabled": i < 4, "capture": 0, "ww": 1,
                          "dl": 0, "dr": 0, "dc": 0, "gain": 32767})
        return {"run_id": 0, "label": "unlabeled", "angle_deg": 0, "distance_cm": 0,
                "phrase": "hey vision", "environment": "simulated", "preset": "raw_vs_bf_lr",
                "live_path": 3, "reduce": "lrc_average", "capture_mode": "event_window",
                "capture_seconds": 2, "raw": {"left": 1, "right": 1, "center": 0},
                "duration_s": 8, "threshold_x1000": 700, "cooldown_ms": 1200, "vote": "4/6",
                "sample_rate": SAMPLE_RATE, "frame": FRAME, "window": 160, "model": "94081",
                "fw": "fake-1.0", "channels": 2, "paths": paths}

    # ---- emit helpers
    def _emit(self, ptype, payload=b"", **kw):
        pkt = sp.Packet(ptype, kw.get("flags", 0), kw.get("run_id", self.run_id),
                        kw.get("stream_id", sp.STREAM_NONE), kw.get("seq", 0),
                        kw.get("timestamp", int(now() * 1000) & 0xFFFFFFFF), payload)
        self.rx_bytes += sp.HEADER_LEN + len(payload) + 2
        self.last_rx = now()
        self.on_event(("packet", pkt))

    def _emit_json(self, ptype, obj, **kw):
        if self.binary:
            self._emit(ptype, json.dumps(obj, separators=(",", ":")).encode(), **kw)
        else:
            self.on_event(("text", f"{sp.TYPE_NAMES[ptype]} {json.dumps(obj)}"))

    def _emit_text(self, line, ptype=sp.TEXT):
        if self.binary:
            self._emit(ptype, line.encode())
        else:
            self.on_event(("text", line))

    def _ack(self, ok, msg):
        self._emit_text(("ok: " if ok else "err: ") + msg, sp.ACK)

    def _hello(self):
        self._emit_json(sp.HELLO, {"device": "simulated nrf54lm20dk", "fw": "fake-1.0",
                                   "build": "sim", "sample_rate": SAMPLE_RATE, "frame": FRAME,
                                   "channels": 2, "window": 160, "model": "94081",
                                   "max_delay": 64, "link_bps": 80000, "tx_ring": 32768,
                                   "queue_depth": 8, "paths": sp.PATH_NAMES,
                                   "path_stream_base": sp.STREAM_PATH_BASE, "state": 0})

    # ---- host -> device
    def send_line(self, line: str) -> None:
        threading.Thread(target=self._handle, args=(line.strip(),), daemon=True).start()

    def send_raw(self, data: bytes) -> None:
        for kind, obj in self.parser.feed(data):
            if kind == "packet":
                if obj.type == sp.COMMAND:
                    self._handle(obj.text().strip())
                elif obj.type == sp.REPLAY_AUDIO:
                    self._replay_frame(obj)
                elif obj.type == sp.REPLAY_END:
                    self._replay_end()
            elif kind == "text":
                self._handle(obj.strip())

    def _handle(self, line: str) -> None:
        if not line:
            return
        parts = line.split()
        cmd, args = parts[0], parts[1:]
        cfg = self.cfg
        if cmd == "binary_mode":
            self.binary = args[:1] == ["on"]
            self._ack(True, f"binary_mode {'on' if self.binary else 'off'}")
            if self.binary:
                self._hello()
                self._emit_json(sp.RUN_CONFIG, cfg)
        elif cmd == "hello":
            self._hello()
            self._ack(True, "hello")
        elif cmd == "config":
            self._emit_json(sp.RUN_CONFIG, cfg)
            self._ack(True, "config")
        elif cmd == "status":
            self._status()
            self._ack(True, "status")
        elif cmd == "help":
            self._emit_text("commands: (simulated) same as firmware")
            self._ack(True, "help")
        elif cmd == "list_paths":
            for p in cfg["paths"]:
                self._emit_text(f"{p['id']} {p['name']:<11} {'yes' if p['enabled'] else 'no'}")
            self._ack(True, "list_paths")
        elif cmd == "start":
            if self.state != "idle":
                self._ack(False, "start: not idle")
            else:
                self._ack(True, "start queued")
                threading.Thread(target=self._run, daemon=True).start()
        elif cmd == "stop":
            if self.state == "idle":
                self._ack(False, "nothing running")
            else:
                self._stop_req = True
                self._ack(True, "stop queued")
        elif cmd == "set_preset" and args:
            masks = {"left": [0], "right": [1], "lr_mix": [2], "bf_lr": [3], "raw_3ch": [4],
                     "bf_3ch": [5], "raw_vs_bf_lr": [0, 1, 2, 3], "raw_vs_bf_3ch": [4, 5],
                     "full": [0, 1, 2, 3, 4, 5]}
            if args[0] not in masks:
                self._ack(False, "unknown preset")
                return
            for p in cfg["paths"]:
                p["enabled"] = 1 if p["id"] in masks[args[0]] else 0
            cfg["preset"] = args[0]
            if not cfg["paths"][cfg["live_path"]]["enabled"]:
                cfg["live_path"] = max(masks[args[0]])
            self._ack(True, f"preset {args[0]}")
        elif cmd == "set_live_path" and args:
            p = sp.PATH_NAMES.index(args[0]) if args[0] in sp.PATH_NAMES else int(args[0])
            cfg["live_path"] = p
            cfg["paths"][p]["enabled"] = 1
            self._ack(True, f"live path {sp.PATH_NAMES[p]}")
        elif cmd in ("enable", "disable") and args:
            p = sp.PATH_NAMES.index(args[0])
            cfg["paths"][p]["enabled"] = 1 if cmd == "enable" else 0
            self._ack(True, f"{cmd} {args[0]}")
        elif cmd == "set_duration" and args:
            cfg["duration_s"] = int(args[0])
            self._ack(True, f"duration {args[0]} s")
        elif cmd == "set_delay" and len(args) >= 3:
            p = sp.PATH_NAMES.index(args[0])
            cfg["paths"][p]["dl"], cfg["paths"][p]["dr"] = int(args[1]), int(args[2])
            cfg["paths"][p]["dc"] = int(args[3]) if len(args) > 3 else 0
            self._ack(True, f"{args[0]} delays set")
        elif cmd == "set_gain" and len(args) >= 2:
            cfg["paths"][sp.PATH_NAMES.index(args[0])]["gain"] = int(args[1])
            self._ack(True, "gain set")
        elif cmd == "set_threshold" and args:
            v = int(float(args[0]) * 1000 + 0.5) if "." in args[0] else int(args[0])
            cfg["threshold_x1000"] = v
            self._ack(True, f"threshold {v}/1000")
        elif cmd == "set_cooldown" and args:
            cfg["cooldown_ms"] = int(args[0])
            self._ack(True, "cooldown set")
        elif cmd == "set_capture" and len(args) >= 2:
            on = args[1] in ("on", "1")
            if args[0] in ("raw_left", "raw_right", "raw_center"):
                cfg["raw"][args[0][4:]] = 1 if on else 0
            else:
                cfg["paths"][sp.PATH_NAMES.index(args[0])]["capture"] = 1 if on else 0
            self._ack(True, f"capture {args[0]} {'on' if on else 'off'}")
        elif cmd == "set_capture_mode" and args:
            cfg["capture_mode"] = args[0]
            self._ack(True, f"capture mode {args[0]}")
        elif cmd == "set_capture_seconds" and args:
            cfg["capture_seconds"] = int(args[0])
            self._ack(True, "capture seconds set")
        elif cmd == "set_reduce" and args:
            cfg["reduce"] = args[0]
            self._ack(True, "reduce set")
        elif cmd == "set_angle" and args:
            cfg["angle_deg"] = int(args[0])
            self._ack(True, f"angle {args[0]}")
        elif cmd == "set_distance" and args:
            cfg["distance_cm"] = int(args[0])
            self._ack(True, f"distance {args[0]} cm")
        elif cmd in ("set_label", "set_phrase", "set_env") and args:
            key = {"set_label": "label", "set_phrase": "phrase", "set_env": "environment"}[cmd]
            cfg[key] = "_".join(args)
            self._ack(True, f"{key} {cfg[key]}")
        elif cmd == "replay_start" and len(args) >= 2:
            if self.state != "idle":
                self._ack(False, "not idle")
                return
            path = sp.PATH_NAMES.index(args[0])
            self._replay = {"path": path, "expected": int(args[1]), "received": 0,
                            "consumed": 0, "frames": [], "sample": 0, "seq": 0,
                            "det": 0, "windows": 0, "peak": 0.0, "sum": 0.0, "crc": 0,
                            "last_det": -1}
            self.state = "replay"
            self._ack(True, f"replay of {args[0]} started")
            self._emit_json(sp.RUN_START, {"run_id": self.run_id, "replay": 1,
                                           "replay_path": path, "name": args[0],
                                           "expected_frames": int(args[1]),
                                           "threshold_x1000": cfg["threshold_x1000"],
                                           "cooldown_ms": cfg["cooldown_ms"]},
                            flags=sp.FLAG_REPLAY, stream_id=sp.path_stream_id(path))
            self._emit_json(sp.REPLAY_ACK, {"consumed": 0, "credits": 8}, flags=sp.FLAG_REPLAY)
        elif cmd == "replay_end":
            self._replay_end()
        else:
            self._ack(False, f"unknown command '{cmd}'")

    # ---- synthetic audio
    def _synth_frame(self, t0: float, burst: bool):
        l, r = [], []
        for i in range(FRAME):
            t = t0 + i / SAMPLE_RATE
            noise = self._rng.gauss(0, 12)
            if burst:
                env = 0.5 + 0.5 * math.sin(2 * math.pi * 3.1 * t)
                sig = 2600 * env * (math.sin(2 * math.pi * 210 * t) * 0.6
                                    + math.sin(2 * math.pi * 640 * t) * 0.3
                                    + self._rng.gauss(0, 0.25))
            else:
                sig = 0.0
            l.append(int(max(-32768, min(32767, sig + noise))))
            r.append(int(max(-32768, min(32767, sig * 0.9 + self._rng.gauss(0, 12)))))
        return l, r

    def _status(self):
        self._emit_json(sp.STATUS, {"state": self.state, "run_id": self.run_id,
                                    "uptime_ms": int(now() * 1000) & 0xFFFFFFFF,
                                    "elapsed_ms": 0, "binary": int(self.binary),
                                    "live_path": self.cfg["live_path"],
                                    "cap": {"captured": 0, "dropped": 0, "discarded": 0,
                                            "qhw": 2, "qdepth": 8, "i2s_err": 0},
                                    "tx": {"sent": 0, "dropped": 0, "audio_dropped": 0,
                                           "ring_hw": 4096, "ring": 32768, "ring_used": 512},
                                    "replay": {"path": -1, "expected": 0, "received": 0,
                                               "consumed": 0, "dropped": 0}})

    def _path_metrics(self, path, m, final=False, replay=False):
        n = max(1, m["windows"])
        self._emit_json(sp.PATH_METRICS, {
            "path": path, "name": sp.PATH_NAMES[path], "enabled": 1,
            "scored": 1 if m.get("scored") else 0, "live": 1 if path == self.cfg["live_path"] else 0,
            "frames": m["frames"], "samples": m["frames"] * FRAME, "windows": m["windows"],
            "det": m["det"], "cool": 0, "miss": 0, "err": 0,
            "last_x1000": int(m["last"] * 1000), "peak_x1000": int(m["peak"] * 1000),
            "mean_x1000": int(m["sum"] * 1000 / n), "min_x1000": 0, "std_x1000": 120,
            "last_det_sample": m["last_det"] if m["last_det"] >= 0 else 0xFFFFFFFF,
            "last_win_sample": m["frames"] * FRAME,
            "inf_last_us": 2100, "inf_mean_us": 2050, "inf_max_us": 2400,
            "rms": m["rms"], "peak_abs": m["peak_abs"], "clips": 0,
            "crc": f"{m['crc'] & 0xFFFFFFFF:08x}", "audio_frames": m["frames"],
            "bf": {"dl": self.cfg["paths"][path]["dl"], "dr": self.cfg["paths"][path]["dr"],
                   "dc": 0, "gain": self.cfg["paths"][path]["gain"], "clips": 0}},
            flags=(sp.FLAG_FINAL if final else 0) | (sp.FLAG_REPLAY if replay else 0),
            stream_id=sp.path_stream_id(path))

    def _score(self, m, pcm_samples, sample_index, burst, replay):
        """Very rough stand-in for the model: probability tracks burst energy."""
        rms = math.sqrt(sum(s * s for s in pcm_samples) / len(pcm_samples))
        score = min(0.99, rms / 1400.0) + self._rng.uniform(-0.05, 0.05)
        score = max(0.0, score)
        m["windows"] += 1
        m["last"] = score
        m["sum"] += score
        m["peak"] = max(m["peak"], score)
        if score * 1000 > self.cfg["threshold_x1000"] and \
           (m["last_det"] < 0 or sample_index - m["last_det"] > self.cfg["cooldown_ms"] * 16):
            m["last_det"] = sample_index
            m["det"] += 1
            self._emit_json(sp.WAKEWORD_EVENT, {
                "path": m["path"], "name": sp.PATH_NAMES[m["path"]], "sample": sample_index,
                "ms": sample_index * 1000 // SAMPLE_RATE, "score_x1000": int(score * 1000),
                "peak_x1000": int(m["peak"] * 1000), "n": m["det"]},
                flags=sp.FLAG_REPLAY if replay else 0, seq=m["det"],
                timestamp=sample_index, stream_id=sp.path_stream_id(m["path"]))

    def _new_metrics(self, path):
        return {"path": path, "frames": 0, "windows": 0, "det": 0, "last": 0.0, "peak": 0.0,
                "sum": 0.0, "last_det": -1, "crc": 0, "rms": 0, "peak_abs": 0, "scored": False}

    def _run(self):
        self.run_id += 1
        cfg = self.cfg
        cfg["run_id"] = self.run_id
        self.state = "running"
        self._stop_req = False
        self._emit_json(sp.RUN_START, cfg, flags=0)
        self._ack(True, f"run {self.run_id} started")
        duration = cfg["duration_s"] or 10
        total_frames = int(duration * SAMPLE_RATE / FRAME)
        metrics = {p["id"]: self._new_metrics(p["id"]) for p in cfg["paths"] if p["enabled"]}
        bursts = [(self._rng.uniform(1.0, duration - 1.5), self._rng.uniform(0.6, 1.1))
                  for _ in range(max(1, int(duration / 3)))]
        seqs = collections.Counter()
        bf_state = {}
        acc = {i: [] for i in metrics}
        t_start = now()
        crc_l = crc_r = 0
        for f in range(total_frames):
            if self._stop_req:
                break
            t0 = f * FRAME / SAMPLE_RATE
            burst = any(b <= t0 <= b + d for b, d in bursts)
            l, r = self._synth_frame(t0, burst)
            sample = f * FRAME
            lb, rb = dsp.samples_to_bytes(l), dsp.samples_to_bytes(r)
            crc_l = zlib.crc32(lb, crc_l)
            crc_r = zlib.crc32(rb, crc_r)
            if cfg["capture_mode"] != "off":
                if cfg["raw"]["left"]:
                    self._emit(sp.AUDIO_CHUNK, lb, stream_id=0, seq=seqs[0], timestamp=sample)
                    seqs[0] += 1
                if cfg["raw"]["right"]:
                    # WW_SIM_DROP=1 drops one chunk mid-run so gap handling can be seen
                    if not (os.environ.get("WW_SIM_DROP") == "1" and f == 137):
                        self._emit(sp.AUDIO_CHUNK, rb, stream_id=1, seq=seqs[1], timestamp=sample)
                    seqs[1] += 1
            paths_pcm = {0: l, 1: r, 2: dsp.lr_mix(l, r)}
            pl = cfg["paths"][3]
            if 3 in metrics:
                # stateful across frames, like bf_process() on the device
                paths_pcm[3], _ = self._bf_stateful(bf_state, l, r, [pl["dl"], pl["dr"]], pl["gain"])
            for pid, m in metrics.items():
                if pid not in paths_pcm:
                    continue
                pcm = paths_pcm[pid]
                m["frames"] += 1
                m["crc"] = zlib.crc32(dsp.samples_to_bytes(pcm), m["crc"])
                m["rms"] = int(math.sqrt(sum(s * s for s in pcm) / len(pcm)))
                m["peak_abs"] = max(m["peak_abs"], max(abs(s) for s in pcm))
                if pid == cfg["live_path"]:
                    m["scored"] = True
                    acc[pid] += pcm
                    while len(acc[pid]) >= 160:
                        win, acc[pid] = acc[pid][:160], acc[pid][160:]
                        self._score(m, win, sample + 160, burst, False)
            if f % 62 == 61:
                self._emit_json(sp.AUDIO_STATS, {"streams": [
                    {"id": 0, "rms": metrics[0]["rms"] if 0 in metrics else 0, "peak": 0, "clips": 0},
                    {"id": 1, "rms": metrics[1]["rms"] if 1 in metrics else 0, "peak": 0, "clips": 0}]},
                    timestamp=sample)
                for pid, m in metrics.items():
                    self._path_metrics(pid, m)
                self._status()
            # pace roughly real time (4 frames per sleep)
            if f % 4 == 3:
                target = t_start + (f + 1) * FRAME / SAMPLE_RATE
                delay = target - now()
                if delay > 0:
                    time.sleep(delay)
        for pid, m in metrics.items():
            self._path_metrics(pid, m, final=True)
        reason = "command" if self._stop_req else "duration"
        self._emit_json(sp.RUN_END, {"run_id": self.run_id, "replay": 0, "replay_path": -1,
                                     "reason": reason, "duration_ms": f * FRAME * 1000 // SAMPLE_RATE,
                                     "samples": f * FRAME, "frames": f,
                                     "detections": sum(m["det"] for m in metrics.values()),
                                     "cap_dropped": 0, "cap_discarded": 0, "i2s_err": 0,
                                     "tx_audio_dropped": int(os.environ.get("WW_SIM_DROP") == "1"),
                                     "tx_dropped": int(os.environ.get("WW_SIM_DROP") == "1"),
                                     "gaps": int(os.environ.get("WW_SIM_DROP") == "1"),
                                     "frames_lost": int(os.environ.get("WW_SIM_DROP") == "1"), "deadline_misses": 0,
                                     "replay_expected": 0, "replay_received": 0,
                                     "replay_consumed": 0, "replay_dropped": 0,
                                     "quality": "degraded" if os.environ.get("WW_SIM_DROP") == "1" else "valid"},
                            flags=sp.FLAG_FINAL)
        self._emit_text(f"Run {self.run_id} finished ({reason})")
        self.state = "idle"

    # ---- replay
    def _replay_frame(self, pkt: sp.Packet):
        rp = self._replay
        if rp is None or self.state != "replay":
            return
        vals = struct.unpack(f"<{len(pkt.payload) // 2}h", pkt.payload)
        l, r = list(vals[0::2]), list(vals[1::2])
        rp["received"] += 1
        rp.setdefault("m", self._new_metrics(rp["path"]))
        m = rp["m"]
        m["scored"] = True
        path = rp["path"]
        pc = self.cfg["paths"][path]
        if path == 0:
            out = l
        elif path == 1:
            out = r
        elif path == 2:
            out = dsp.lr_mix(l, r)
        elif path == 3:
            # per-frame delay-and-sum with persistent history
            hist = rp.setdefault("bf_hist", None)
            out, _ = self._bf_stateful(rp, l, r, [pc["dl"], pc["dr"]], pc["gain"])
        else:
            out = dsp.lr_mix(l, r)
        sample = rp["sample"]
        pcm = dsp.samples_to_bytes(out)
        m["frames"] += 1
        m["crc"] = zlib.crc32(pcm, m["crc"])
        m["rms"] = int(math.sqrt(sum(s * s for s in out) / len(out)))
        m["peak_abs"] = max(m["peak_abs"], max(abs(s) for s in out))
        self._emit(sp.AUDIO_CHUNK, pcm, stream_id=sp.path_stream_id(path), seq=rp["seq"],
                   timestamp=sample, flags=sp.FLAG_REPLAY)
        rp["seq"] += 1
        acc = rp.setdefault("acc", [])
        acc += out
        while len(acc) >= 160:
            win, acc[:] = acc[:160], acc[160:]
            self._score(m, win, sample + 160, False, True)
        rp["sample"] += len(out)
        rp["consumed"] += 1
        if rp["consumed"] % 4 == 0 or rp["consumed"] == rp["expected"]:
            self._emit_json(sp.REPLAY_ACK, {"consumed": rp["consumed"], "credits": 8},
                            flags=sp.FLAG_REPLAY, seq=rp["consumed"], timestamp=rp["sample"])

    def _bf_stateful(self, rp, l, r, delays, gain):
        # Reuse dsp.delay_and_sum on a concatenation of history + frame for exactness.
        hist = rp.get("bf_hist_lr")
        if hist is None:
            hist = ([0] * 64, [0] * 64)
        cl = hist[0] + l
        cr = hist[1] + r
        out, clips = dsp.delay_and_sum([cl, cr], delays, [gain, gain], frame=len(cl))
        rp["bf_hist_lr"] = (cl[-64:], cr[-64:])
        return out[64:], clips

    def _replay_end(self):
        rp = self._replay
        if rp is None or self.state != "replay":
            self._ack(False, "no replay in progress")
            return
        m = rp.get("m", self._new_metrics(rp["path"]))
        self._path_metrics(rp["path"], m, final=True, replay=True)
        self._emit_json(sp.RUN_END, {"run_id": self.run_id, "replay": 1, "replay_path": rp["path"],
                                     "reason": "replay_end", "duration_ms": rp["sample"] * 1000 // SAMPLE_RATE,
                                     "samples": rp["sample"], "frames": rp["consumed"],
                                     "detections": m["det"], "cap_dropped": 0, "cap_discarded": 0,
                                     "i2s_err": 0, "tx_audio_dropped": 0, "tx_dropped": 0, "gaps": 0,
                                     "frames_lost": 0, "deadline_misses": 0,
                                     "replay_expected": rp["expected"], "replay_received": rp["received"],
                                     "replay_consumed": rp["consumed"], "replay_dropped": 0,
                                     "quality": "valid"}, flags=sp.FLAG_FINAL | sp.FLAG_REPLAY)
        self._ack(True, "replay_end queued")
        self.state = "idle"
        self._replay = None


# ================================================================ session

class Session:
    """All device-side knowledge, run recording, and the API's view of the world."""

    def __init__(self, runs_dir: str, auto_replay: bool):
        self.lock = threading.RLock()
        self.runs_dir = runs_dir
        self.auto_replay = auto_replay
        os.makedirs(runs_dir, exist_ok=True)
        self.device = None
        self.device_info: Dict[str, Any] = {}
        self.config: Dict[str, Any] = {}
        self.status: Dict[str, Any] = {}
        self.log = collections.deque(maxlen=200)
        self.acks = collections.deque(maxlen=40)
        self.connected = False
        self.tracker = sp.StreamTracker()
        self.run: Dict[str, Any] = self._empty_run()
        self.folder: Optional[run_storage.RunFolder] = None
        self.writers: Dict[int, wav_writer.StreamWriter] = {}
        self.replay_writer: Optional[wav_writer.StreamWriter] = None
        self.replay_job: Optional[ReplayJob] = None
        self.replay_started = threading.Event()
        self.replay_ended = threading.Event()
        self.replay_ack = threading.Event()
        self.replay_consumed = 0
        self.replay_credits = 8
        self._rx_hist = collections.deque(maxlen=20)

    def _empty_run(self):
        return {"active": False, "phase": "idle", "run_dir": None, "run_id": 0, "config": {},
                "start_time": None, "elapsed_ms": 0, "paths": {}, "replay_paths": {},
                "streams": {}, "events": [], "gaps": [], "warnings": [], "audio_files": [],
                "quality": None, "end": {}, "replay_job": None, "replay_path": None}

    # ---- device event entry point
    def on_event(self, ev):
        kind, obj = ev
        with self.lock:
            if kind == "connected":
                self.connected = True
                self._log("[host] serial connected")
            elif kind == "disconnected":
                self.connected = False
                self._log(f"[host] serial disconnected: {obj}")
                if self.run["active"]:
                    self._abort_run("device disconnected before RUN_END")
            elif kind == "log":
                self._log(str(obj))
            elif kind == "text":
                self._log(obj)
                if obj.startswith(("ok:", "err:")):
                    self.acks.append(obj)
            elif kind == "bad_crc":
                self._log("[host] packet with bad CRC")
            elif kind == "packet":
                self._on_packet(obj)

    def _log(self, line: str):
        self.log.append(f"{time.strftime('%H:%M:%S')} {line}")
        if self.folder:
            self.folder.add_log(line)

    def _on_packet(self, p: sp.Packet):
        t = p.type
        if t in (sp.LOG, sp.TEXT, sp.ERROR):
            self._log(("[dev] " if t == sp.LOG else "") + p.text())
        elif t == sp.ACK:
            self.acks.append(p.text())
            self._log(p.text())
        elif t == sp.HELLO:
            self.device_info = jload(p)
        elif t == sp.RUN_CONFIG:
            self.config = jload(p)
        elif t == sp.STATUS:
            self.status = jload(p)
            if self.run["active"] and self.run["start_time"]:
                self.run["elapsed_ms"] = int((now() - self.run["start_time"]) * 1000)
        elif t == sp.RUN_START:
            self._on_run_start(p)
        elif t == sp.AUDIO_CHUNK:
            self._on_audio(p)
        elif t == sp.AUDIO_GAP:
            g = jload(p)
            self._record_gap(g.get("stream", p.stream_id), g.get("frames_lost", 0),
                             g.get("from_sample", p.timestamp), "device: " + g.get("reason", ""),
                             p.is_replay)
        elif t == sp.AUDIO_STATS:
            for s in jload(p).get("streams", []):
                name = sp.stream_name(s.get("id", -1))
                self.run["streams"][name] = {**s, "name": name, "sample_index": p.timestamp}
                if self.folder and self.run["active"]:
                    self.folder.add_stats({"run_id": p.run_id, "replay": int(p.is_replay),
                                           "sample_index": p.timestamp, "stream": s.get("id"),
                                           "name": name, "rms": s.get("rms"), "peak": s.get("peak"),
                                           "clips": s.get("clips")})
        elif t == sp.WAKEWORD_EVENT:
            e = jload(p)
            e["replay"] = p.is_replay
            e["host_time"] = now()
            self.run["events"].append(e)
            self.run["events"] = self.run["events"][-500:]
            if self.folder:
                self.folder.add_event("WAKEWORD_EVENT", e, replay=p.is_replay)
        elif t == sp.PATH_METRICS:
            m = jload(p)
            m["final"] = p.is_final
            m["replay"] = p.is_replay
            name = m.get("name", sp.stream_name(p.stream_id))
            (self.run["replay_paths"] if p.is_replay else self.run["paths"])[name] = m
            if self.folder and (self.run["active"] or p.is_replay):
                self.folder.add_metrics({"run_id": p.run_id, "replay": int(p.is_replay),
                                         "sample_index": p.timestamp, **m})
        elif t == sp.RUN_END:
            self._on_run_end(p)
        elif t == sp.REPLAY_ACK:
            a = jload(p)
            self.replay_consumed = a.get("consumed", 0)
            self.replay_credits = a.get("credits", 8)
            self.replay_ack.set()

    # ---- run lifecycle
    def _on_run_start(self, p: sp.Packet):
        cfg = jload(p)
        if p.is_replay:
            self.run["phase"] = "replay"
            self.run["replay_path"] = cfg.get("name", sp.PATH_NAMES[cfg.get("replay_path", 0)])
            self.replay_writer = wav_writer.StreamWriter(self.run["replay_path"], SAMPLE_RATE, 1)
            self.replay_consumed = 0
            self.replay_started.set()
            self._log(f"[host] replay pass started for {self.run['replay_path']}")
            return
        if self.run["active"]:
            self._abort_run("new run started before the previous one ended")
        self.tracker.reset()
        self.writers = {}
        self.run = self._empty_run()
        self.run.update({"active": True, "phase": "running", "run_id": cfg.get("run_id", p.run_id),
                         "config": cfg, "start_time": now()})
        self.folder = run_storage.RunFolder(self.runs_dir, cfg)
        self.folder.metadata["device"] = self.device_info
        self.folder.metadata["host"] = {"tool": "dashboard_server.py", "python": sys.version.split()[0]}
        self.folder.add_event("RUN_START", cfg)
        self.run["run_dir"] = self.folder.name
        self._log(f"[host] run {self.run['run_id']} -> {self.folder.name}")

    def _on_audio(self, p: sp.Packet):
        gap = self.tracker.observe(p)
        if gap:
            self._record_gap(p.stream_id, gap.frames_lost, gap.sample_index, "missing sequence numbers",
                             p.is_replay)
        if p.is_replay:
            if self.replay_writer is not None:
                self.replay_writer.add(p.timestamp, p.payload)
            return
        if not self.run["active"]:
            return
        w = self.writers.get(p.stream_id)
        if w is None:
            w = self.writers[p.stream_id] = wav_writer.StreamWriter(sp.stream_name(p.stream_id),
                                                                    SAMPLE_RATE, 1)
        w.add(p.timestamp, p.payload)

    def _record_gap(self, stream_id, frames_lost, sample_index, reason, replay):
        g = {"stream": sp.stream_name(stream_id), "frames_lost": frames_lost,
             "sample_index": sample_index, "reason": reason, "replay": replay}
        self.run["gaps"].append(g)
        self._warn(f"audio gap on {g['stream']}: {frames_lost} frame(s) lost ({reason})")
        if self.folder:
            self.folder.add_event("AUDIO_GAP", g, replay=replay)

    def _warn(self, text: str):
        if text not in self.run["warnings"]:
            self.run["warnings"].append(text)
        if self.folder:
            self.folder.warn(text)

    def _abort_run(self, reason: str):
        self._warn(reason)
        if self.folder:
            self._finalize_live({"reason": "aborted", "quality": "degraded"}, completion="aborted")

    def _on_run_end(self, p: sp.Packet):
        end = jload(p)
        if p.is_replay:
            self._finalize_replay(end)
            return
        if not self.run["active"]:
            return
        self._finalize_live(end)

    def _capture_policy_files(self, cfg, events):
        """Return list of (writer, filename, start, end) to write according to the
        capture policy. The full raw channels are always kept: they are the
        source for replay scoring and for host reconstruction."""
        mode = cfg.get("capture_mode", "continuous_full")
        secs = int(cfg.get("capture_seconds", 2) or 2)
        out = []
        for sid, w in sorted(self.writers.items()):
            if mode == "continuous_short":
                out.append((w, f"{w.name}.wav", 0, secs * SAMPLE_RATE))
            else:
                out.append((w, f"{w.name}.wav", 0, None))
            if mode == "event_window":
                for i, e in enumerate([e for e in events if not e.get("replay")], 1):
                    s = int(e.get("sample", 0)) - w.first_sample_index
                    out.append((w, f"{w.name}_event{i:02d}.wav",
                                max(0, s - secs * SAMPLE_RATE), s + secs * SAMPLE_RATE))
        return out

    def _finalize_live(self, end: Dict[str, Any], completion: str = "complete"):
        folder = self.folder
        cfg = self.run["config"]
        self.run["phase"] = "finalizing"
        self.run["end"] = end
        folder.add_event("RUN_END", end)
        files: List[Dict[str, Any]] = []

        # 1. audio artifacts from the device
        if self.writers:
            for w, fname, s, e in self._capture_policy_files(cfg, self.run["events"]):
                info = w.write(folder.file(fname), s, e)
                files.append(info)
            if 0 in self.writers and 1 in self.writers:
                l, r = self.writers[0].pcm_bytes(), self.writers[1].pcm_bytes()
                st = wav_writer.interleave(l, r)
                wav_writer.write_wav(folder.file("raw_stereo.wav"), st, SAMPLE_RATE, 2)
                files.append({"file": folder.file("raw_stereo.wav"), "stream": "raw_stereo",
                              "channels": 2, "samples": len(st) // 4,
                              "duration_s": round(len(st) / 4 / SAMPLE_RATE, 3),
                              "complete": self.writers[0].complete and self.writers[1].complete,
                              "gaps": []})
            for w in self.writers.values():
                if not w.complete:
                    self._warn(f"WAV {w.name} has {len(w.gaps)} gap(s); marked incomplete")
        elif cfg.get("capture_mode", "off") != "off":
            self._warn("no audio was received although capture was enabled")

        # 2. host reconstruction of processed paths, verified against device CRCs
        crc_host: Dict[str, str] = {}
        if 0 in self.writers and 1 in self.writers:
            lb, rb = self.writers[0].pcm_bytes(), self.writers[1].pcm_bytes()
            cb = self.writers[2].pcm_bytes() if 2 in self.writers else None
            for pc in cfg.get("paths", []):
                name = pc["name"]
                if not pc.get("enabled") or name in ("raw_left", "raw_right"):
                    continue
                try:
                    pcm, info = dsp.build_path(name, lb, rb, cb, cfg)
                except ValueError as ex:
                    self._warn(f"{name}: not reconstructed ({ex})")
                    continue
                wav_writer.write_wav(folder.file(f"{name}.wav"), pcm, SAMPLE_RATE, 1)
                crc_host[name] = info["crc32"]
                dev = self.run["paths"].get(name, {}).get("crc")
                match = (dev == info["crc32"]) if dev else None
                if match is False:
                    self._warn(f"{name}: host reconstruction CRC {info['crc32']} != device CRC {dev}"
                               " (raw audio has gaps, or DSP parameters differ)")
                files.append({"file": folder.file(f"{name}.wav"), "stream": name, "channels": 1,
                              "samples": info["samples"],
                              "duration_s": round(info["samples"] / SAMPLE_RATE, 3),
                              "complete": self.writers[0].complete and self.writers[1].complete,
                              "gaps": [], "reconstructed": True, "crc_host": info["crc32"],
                              "crc_device": dev, "crc_match": match, "params": info})

        # 3. quality and metadata
        q = end.get("quality", "unknown")
        if self.run["gaps"] or any(f.get("crc_match") is False for f in files):
            q = "degraded"
        if end.get("deadline_misses"):
            self._warn("results may not be comparable: inference deadline misses")
        if end.get("cap_dropped"):
            self._warn("results may not be comparable: dropped capture frames")
        self.run["quality"] = q
        self.run["audio_files"] = [{**f, "file": os.path.basename(f["file"])} for f in files]
        folder.metadata["audio_files"] = self.run["audio_files"]
        folder.metadata["end"] = end
        folder.metadata["quality"] = q
        folder.metadata["live_metrics"] = self.run["paths"]
        folder.metadata["gaps"] = self.run["gaps"]
        folder.metadata["crc_host"] = crc_host
        self._write_summary(folder)
        write_report(folder.path)
        folder.close(completion)
        self.folder = None
        self.run["active"] = False
        self.run["phase"] = "idle"
        self._log(f"[host] run saved: {folder.name} ({q})")

        # 4. replay the recording through the device for every other path
        live_name = sp.PATH_NAMES[cfg.get("live_path", 3)]
        have_center = 2 in self.writers
        to_replay = []
        for pc in cfg.get("paths", []):
            name = pc["name"]
            if not pc.get("enabled") or not pc.get("ww", 1) or name == live_name:
                continue
            if name in ("raw_3ch", "bf_3ch") and not have_center:
                continue
            to_replay.append(name)
        if self.auto_replay and to_replay and 0 in self.writers and 1 in self.writers \
                and completion == "complete":
            self.start_replay(folder.path, to_replay)
        elif to_replay:
            self._warn("paths not scored: " + ", ".join(to_replay) + " (run replay from the dashboard)")

    def _write_summary(self, folder):
        cfg = self.run["config"]
        meta = folder.metadata
        rows = []
        live_name = sp.PATH_NAMES[cfg.get("live_path", 3)]
        for pc in cfg.get("paths", []):
            name = pc["name"]
            m = self.run["paths"].get(name, {})
            rp = meta.get("replays", {}).get(name, {}).get("metrics", {})
            src = m
            scored_by = "none"
            if name == live_name and m.get("scored"):
                scored_by = "live"
            elif rp:
                src, scored_by = rp, "replay"
            crc_host = meta.get("crc_host", {}).get(name)
            rows.append({
                "path": pc["id"], "name": name, "enabled": pc.get("enabled", 0),
                "scored": 1 if scored_by != "none" else 0, "scored_by": scored_by,
                "detections": src.get("det", ""), "cooldown_suppressed": src.get("cool", ""),
                "peak_x1000": src.get("peak_x1000", ""), "mean_x1000": src.get("mean_x1000", ""),
                "min_x1000": src.get("min_x1000", ""), "std_x1000": src.get("std_x1000", ""),
                "last_x1000": src.get("last_x1000", ""), "windows": src.get("windows", ""),
                "frames": m.get("frames", src.get("frames", "")), "samples": m.get("samples", ""),
                "rms": m.get("rms", src.get("rms", "")), "peak_abs": m.get("peak_abs", ""),
                "clips": m.get("clips", ""), "inf_mean_us": src.get("inf_mean_us", ""),
                "inf_max_us": src.get("inf_max_us", ""), "deadline_misses": src.get("miss", ""),
                "errors": src.get("err", ""), "crc": m.get("crc", ""), "crc_host": crc_host or "",
                "crc_match": "" if not crc_host or not m.get("crc") else int(crc_host == m.get("crc")),
                "bf_dl": pc.get("dl", 0), "bf_dr": pc.get("dr", 0), "bf_dc": pc.get("dc", 0),
                "bf_gain": pc.get("gain", 32767), "bf_clips": (m.get("bf") or {}).get("clips", ""),
            })
        folder.write_summary(rows)
        meta["summary"] = rows

    # ---- replay
    def start_replay(self, run_path: str, paths: List[str]) -> bool:
        if self.replay_job and self.replay_job.is_alive():
            return False
        if self.run["active"]:
            return False
        meta = run_storage.load_metadata(run_path)
        folder = _reopen_folder(run_path, meta)
        with self.lock:
            self.folder = folder
            self.run["run_dir"] = folder.name
            self.run["config"] = meta.get("config", self.run.get("config", {}))
            self.run["audio_files"] = list(meta.get("audio_files", []))
            self.run["replay_paths"] = {}
            self.run["events"] = [e for e in self.run["events"] if not e.get("replay")]
        self.replay_job = ReplayJob(self, run_path, paths, folder)
        self.replay_job.start()
        return True

    def _finalize_replay(self, end: Dict[str, Any]):
        name = self.run.get("replay_path")
        folder = self.folder
        if folder and name:
            info = None
            if self.replay_writer and self.replay_writer.samples_written:
                info = self.replay_writer.write(folder.file(f"{name}_device_replay.wav"))
                info["file"] = os.path.basename(info["file"])
                info["replay"] = True
            m = self.run["replay_paths"].get(name, {})
            host_crc = folder.metadata.get("crc_host", {}).get(name)
            dev_crc = m.get("crc")
            rep = {"metrics": m, "end": end, "events": [e for e in self.run["events"]
                                                       if e.get("replay") and e.get("name") == name],
                   "audio_file": info, "crc_device": dev_crc, "crc_host_reconstruction": host_crc,
                   "crc_match": (dev_crc == host_crc) if (dev_crc and host_crc) else None,
                   "detections": m.get("det")}
            if rep["crc_match"] is False:
                folder.warn(f"{name}: replay CRC {dev_crc} != host reconstruction {host_crc}")
            if end.get("replay_dropped") or end.get("replay_consumed", 0) != end.get("replay_expected", 0):
                folder.warn(f"{name}: replay incomplete ({end.get('replay_consumed')} of "
                            f"{end.get('replay_expected')} frames consumed)")
            folder.metadata.setdefault("replays", {})[name] = rep
            if info:
                folder.metadata["audio_files"].append(info)
                self.run["audio_files"].append(info)
            self._write_summary(folder)
            folder.save_metadata()
            write_report(folder.path)
            self._log(f"[host] replay of {name} saved: {m.get('det')} detections")
        self.run["phase"] = "idle"
        self.replay_ended.set()

    # ---- API snapshot
    def snapshot(self) -> Dict[str, Any]:
        with self.lock:
            dev = self.device
            rx_rate = 0.0
            if dev:
                self._rx_hist.append((now(), dev.rx_bytes))
                if len(self._rx_hist) >= 2:
                    (t0, b0), (t1, b1) = self._rx_hist[0], self._rx_hist[-1]
                    rx_rate = (b1 - b0) / max(1e-3, t1 - t0)
            run = dict(self.run)
            run["events"] = self.run["events"][-300:]
            if self.replay_job and self.replay_job.is_alive():
                run["replay_job"] = self.replay_job.progress()
            return {
                "time": now(),
                "connection": {
                    "connected": bool(dev and dev.connected), "binary": bool(dev and dev.binary),
                    "port": getattr(dev, "port", "simulated"), "baud": getattr(dev, "baud", 0),
                    "simulated": isinstance(dev, FakeDevice), "rx_bytes_s": int(rx_rate),
                    "last_rx_ago_s": round(now() - dev.last_rx, 1) if dev and dev.last_rx else None,
                    "bad_crc": dev.parser.bad_crc if dev else 0,
                    "resyncs": dev.parser.resyncs if dev else 0,
                },
                "device": self.device_info, "config": self.config, "status": self.status,
                "run": run, "log": list(self.log), "acks": list(self.acks),
                "enums": {"paths": sp.PATH_NAMES, "presets": PRESETS,
                          "capture_modes": CAPTURE_MODES, "reduce_modes": REDUCE_MODES},
            }


class ReplayJob(threading.Thread):
    """Streams a run's raw L/R recording back through the device once per path."""

    def __init__(self, session: Session, run_path: str, paths: List[str], folder):
        super().__init__(name="replay-job", daemon=True)
        self.s = session
        self.run_path = run_path
        self.paths = paths
        self.index = 0
        self.sent = 0
        self.total = 0
        self.error = None
        self.folder = folder
        self.current = None

    def progress(self):
        return {"paths": self.paths, "index": self.index, "path": self.current,
                "sent": self.sent, "consumed": self.s.replay_consumed, "total": self.total,
                "error": self.error}

    def run(self):
        s = self.s
        try:
            l, _, _ = wav_writer.read_wav(os.path.join(self.run_path, "raw_left.wav"))
            r, _, _ = wav_writer.read_wav(os.path.join(self.run_path, "raw_right.wav"))
            frames = list(sp.iter_frames(l, r, FRAME))
            self.total = len(frames)
            for i, path in enumerate(self.paths):
                self.index = i
                self.current = path
                self.sent = 0
                with s.lock:
                    s.run["phase"] = "replay"
                    s.run["replay_path"] = path
                    s.replay_started.clear()
                    s.replay_ended.clear()
                    s.replay_ack.clear()
                    s.replay_consumed = 0
                s.device.send_line(f"replay_start {path} {len(frames)}")
                if not s.replay_started.wait(10):
                    raise RuntimeError(f"device did not start replay for {path}")
                window = 8
                for idx, data in frames:
                    while (self.sent - s.replay_consumed) >= window:
                        s.replay_ack.clear()
                        if not s.replay_ack.wait(5):
                            raise RuntimeError(f"replay stalled at frame {idx} ({path})")
                    s.device.send_raw(sp.encode_replay_audio(idx, data))
                    self.sent += 1
                deadline = now() + 10
                while s.replay_consumed < len(frames) and now() < deadline:
                    s.replay_ack.clear()
                    s.replay_ack.wait(1)
                s.device.send_raw(sp.encode_replay_end())
                if not s.replay_ended.wait(15):
                    raise RuntimeError(f"device did not finish replay for {path}")
            self.index = len(self.paths)
        except Exception as e:  # noqa: BLE001
            self.error = str(e)
            with s.lock:
                s._log(f"[host] replay job failed: {e}")
                if self.folder:
                    self.folder.warn(f"replay failed: {e}")
                    self.folder.save_metadata()
                s.run["phase"] = "idle"
                try:
                    s.device.send_line("replay_end")
                except Exception:  # noqa: BLE001
                    pass
        finally:
            with s.lock:
                if self.folder and s.folder is self.folder:
                    self.folder.close(self.folder.metadata.get("completion", "complete"))
                    s.folder = None
                s._log("[host] replay job finished" + (f" with error: {self.error}" if self.error else ""))


def _reopen_folder(run_path: str, meta: Dict[str, Any]) -> run_storage.RunFolder:
    """Re-attach a RunFolder to an existing directory without recreating it."""
    rf = run_storage.RunFolder.__new__(run_storage.RunFolder)
    rf.root = os.path.dirname(run_path)
    rf.name = os.path.basename(run_path)
    rf.path = run_path
    rf.metadata = meta
    rf._events = open(os.path.join(run_path, "events.jsonl"), "a", encoding="utf-8")
    rf._log = open(os.path.join(run_path, "device_serial_log.txt"), "a", encoding="utf-8")
    rf._metrics = rf._csv("metrics_timeseries.csv", run_storage.METRICS_FIELDS)
    rf._stats = rf._csv("audio_stats.csv", run_storage.STATS_FIELDS)
    rf.closed = False
    return rf


# ================================================================= report

def write_report(run_path: str) -> str:
    meta = run_storage.load_metadata(run_path)
    summary = run_storage.load_summary(run_path)
    events = [e for e in run_storage.load_events(run_path) if e.get("type") == "WAKEWORD_EVENT"]
    cfg = meta.get("config", {})
    e = html.escape

    def row(cells, tag="td"):
        return "<tr>" + "".join(f"<{tag}>{e(str(c))}</{tag}>" for c in cells) + "</tr>"

    cols = ["name", "enabled", "scored_by", "detections", "peak_x1000", "mean_x1000", "rms",
            "clips", "inf_mean_us", "inf_max_us", "deadline_misses", "crc_match"]
    parts = [
        "<!doctype html><meta charset=utf-8><title>Run report</title>",
        "<style>body{font-family:system-ui,sans-serif;max-width:1100px;margin:24px auto;padding:0 16px;"
        "color:#1f2933}table{border-collapse:collapse;margin:8px 0 20px}td,th{border:1px solid #cfd6de;"
        "padding:4px 8px;font-size:14px}th{background:#eef1f4;text-align:left}.warn{background:#fff4cc;"
        "padding:8px 12px;border-radius:4px;margin:6px 0}h2{margin-top:28px}</style>",
        f"<h1>Wake-word beamforming evaluation run</h1><p><b>{e(meta.get('run_dir', ''))}</b><br>",
        f"label {e(str(cfg.get('label')))} · angle {cfg.get('angle_deg')}° · distance {cfg.get('distance_cm')} cm · "
        f"preset {e(str(cfg.get('preset')))} · live path {e(sp.PATH_NAMES[cfg.get('live_path', 3)] if isinstance(cfg.get('live_path'), int) else '')} · "
        f"threshold {cfg.get('threshold_x1000')}/1000 · cooldown {cfg.get('cooldown_ms')} ms · "
        f"capture {e(str(cfg.get('capture_mode')))} · quality <b>{e(str(meta.get('quality')))}</b> · "
        f"completion {e(str(meta.get('completion')))}</p>",
    ]
    for w in meta.get("warnings", []):
        parts.append(f"<div class=warn>{e(w)}</div>")
    parts.append("<h2>Path results</h2><table>" + row(cols, "th"))
    for r in summary:
        parts.append(row([r.get(c, "") for c in cols]))
    parts.append("</table>")
    parts.append("<h2>Detections</h2><table>" + row(["path", "time (s)", "score", "peak", "source"], "th"))
    for ev in events:
        parts.append(row([ev.get("name"), f"{ev.get('ms', 0) / 1000:.2f}",
                          f"{ev.get('score_x1000', 0) / 1000:.3f}", f"{ev.get('peak_x1000', 0) / 1000:.3f}",
                          "replay" if ev.get("replay") else "live"]))
    parts.append("</table><h2>Audio files</h2><ul>")
    for f in meta.get("audio_files", []):
        note = "" if f.get("complete", True) else " (INCOMPLETE, gaps)"
        crc = ""
        if f.get("reconstructed"):
            crc = f" host reconstruction, CRC match: {f.get('crc_match')}"
        parts.append(f"<li><a href=\"{e(f['file'])}\">{e(f['file'])}</a> {f.get('duration_s')} s{note}{crc}</li>")
    parts.append("</ul><h2>Beamformer parameters</h2><table>" + row(["path", "dl", "dr", "dc", "gain", "clips"], "th"))
    for r in summary:
        if r.get("name", "").startswith("bf_"):
            parts.append(row([r["name"], r.get("bf_dl"), r.get("bf_dr"), r.get("bf_dc"), r.get("bf_gain"), r.get("bf_clips")]))
    parts.append("</table><p><small>Detection counts are not accuracy. Precision, recall, F1 and latency need "
                 "hand-labeled wake-word timestamps; see README.</small></p>")
    out = os.path.join(run_path, "dashboard_export.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    return out


# =================================================================== HTTP

class Handler(BaseHTTPRequestHandler):
    session: Session = None  # set by main

    def log_message(self, fmt, *args):  # quiet
        pass

    def _send(self, code: int, body: bytes, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, default=str).encode())

    def _file(self, path: str, ctype=None):
        if not os.path.isfile(path):
            return self._json({"error": "not found"}, 404)
        ext = os.path.splitext(path)[1].lower()
        ctype = ctype or {".html": "text/html; charset=utf-8", ".js": "text/javascript",
                          ".css": "text/css", ".wav": "audio/wav", ".json": "application/json",
                          ".csv": "text/csv", ".jsonl": "text/plain", ".txt": "text/plain"}.get(ext, "application/octet-stream")
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    def do_GET(self):
        s = self.session
        u = urlparse(self.path)
        q = parse_qs(u.query)
        parts = [p for p in u.path.split("/") if p]
        if not parts:
            return self._file(os.path.join(STATIC_DIR, "index.html"))
        if parts[0] == "static" and len(parts) == 2:
            return self._file(os.path.join(STATIC_DIR, os.path.basename(parts[1])))
        if parts[0] != "api":
            return self._json({"error": "not found"}, 404)
        if parts[1:] == ["state"]:
            return self._json(s.snapshot())
        if parts[1:] == ["runs"]:
            return self._json(run_storage.list_runs(s.runs_dir))
        if parts[1] == "runs" and len(parts) >= 3:
            run_dir = os.path.basename(parts[2])
            rp = os.path.join(s.runs_dir, run_dir)
            if not os.path.isdir(rp):
                return self._json({"error": "run not found"}, 404)
            if len(parts) == 3:
                return self._json({"metadata": run_storage.load_metadata(rp),
                                   "summary": run_storage.load_summary(rp),
                                   "events": run_storage.load_events(rp),
                                   "files": sorted(os.listdir(rp))})
            if parts[3] == "file" and len(parts) == 5:
                return self._file(os.path.join(rp, os.path.basename(parts[4])))
            if parts[3] == "report":
                return self._file(write_report(rp))
        if parts[1:] == ["compare"]:
            out = {}
            for key in ("a", "b"):
                d = q.get(key, [""])[0]
                rp = os.path.join(s.runs_dir, os.path.basename(d))
                if os.path.isdir(rp):
                    out[key] = {"run_dir": d, "metadata": run_storage.load_metadata(rp),
                                "summary": run_storage.load_summary(rp),
                                "events": [e for e in run_storage.load_events(rp)
                                           if e.get("type") == "WAKEWORD_EVENT"]}
            return self._json(out)
        return self._json({"error": "not found"}, 404)

    def do_POST(self):
        s = self.session
        u = urlparse(self.path)
        parts = [p for p in u.path.split("/") if p]
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = json.loads(self.rfile.read(n) or b"{}") if n else {}
        if parts == ["api", "command"]:
            line = str(body.get("line", "")).strip()
            if not line:
                return self._json({"error": "empty"}, 400)
            s.device.send_line(line)
            return self._json({"ok": True})
        if parts == ["api", "start"]:
            cmds = []
            for key, cmd in (("angle", "set_angle"), ("distance", "set_distance"),
                             ("label", "set_label"), ("phrase", "set_phrase"), ("env", "set_env"),
                             ("preset", "set_preset"), ("live_path", "set_live_path"),
                             ("duration", "set_duration"), ("threshold", "set_threshold"),
                             ("cooldown", "set_cooldown"), ("capture_mode", "set_capture_mode"),
                             ("capture_seconds", "set_capture_seconds"), ("reduce", "set_reduce")):
                v = body.get(key)
                if v not in (None, ""):
                    cmds.append(f"{cmd} {v}")
            for name, on in (body.get("capture") or {}).items():
                cmds.append(f"set_capture {name} {'on' if on else 'off'}")
            for name, d in (body.get("delays") or {}).items():
                cmds.append(f"set_delay {name} " + " ".join(str(int(x)) for x in d))
            for name, on in (body.get("paths") or {}).items():
                cmds.append(f"{'enable' if on else 'disable'} {name}")
            cmds.append("start")
            for c in cmds:
                s.device.send_line(c)
                time.sleep(0.03)
            return self._json({"ok": True, "commands": cmds})
        if parts == ["api", "stop"]:
            s.device.send_line("stop")
            return self._json({"ok": True})
        if parts == ["api", "replay"]:
            run_dir = os.path.basename(str(body.get("run_dir", "")))
            paths = [p for p in body.get("paths", []) if p in sp.PATH_NAMES]
            rp = os.path.join(s.runs_dir, run_dir)
            if not os.path.isdir(rp) or not paths:
                return self._json({"error": "bad run or paths"}, 400)
            if not os.path.isfile(os.path.join(rp, "raw_left.wav")):
                return self._json({"error": "run has no raw_left.wav/raw_right.wav to replay"}, 400)
            ok = s.start_replay(rp, paths)
            return self._json({"ok": ok, "error": None if ok else "replay already running"})
        return self._json({"error": "not found"}, 404)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. /dev/tty.usbmodem0010500000001 or COM5")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--web-port", type=int, default=5000)
    ap.add_argument("--runs-dir", default=os.path.join(os.path.dirname(HERE), "runs"))
    ap.add_argument("--simulate", action="store_true", help="use a synthetic device (no hardware)")
    ap.add_argument("--no-auto-replay", action="store_true",
                    help="do not automatically replay each run through the device for the other paths")
    args = ap.parse_args()

    session = Session(os.path.abspath(args.runs_dir), auto_replay=not args.no_auto_replay)
    if args.simulate:
        session.device = FakeDevice(session.on_event)
        threading.Timer(0.5, lambda: session.device.send_line("binary_mode on")).start()
    elif args.port:
        session.device = SerialDevice(args.port, args.baud, session.on_event)
    else:
        ap.error("give --port <serial device> or --simulate")

    Handler.session = session
    srv = ThreadingHTTPServer(("127.0.0.1", args.web_port), Handler)
    print(f"dashboard: http://localhost:{args.web_port}   runs: {session.runs_dir}")
    print(f"device:    {'simulated' if args.simulate else args.port + ' @ ' + str(args.baud)}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
