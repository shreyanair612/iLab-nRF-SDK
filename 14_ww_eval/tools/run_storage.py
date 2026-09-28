"""
On-disk layout for evaluation runs. No database: one folder per run holding
JSON, CSV, JSONL and WAV files that any tool can read.

runs/
  2026-09-24_101500_angle045_distance100cm_label/
    run_metadata.json        config, device info, host info, completion, warnings
    run_summary.csv          one row per path: detections, scores, health
    events.jsonl             every WAKEWORD_EVENT / AUDIO_GAP / RUN_* as received
    metrics_timeseries.csv   periodic PATH_METRICS rows
    audio_stats.csv          periodic per-stream RMS / peak / clips
    device_serial_log.txt    LOG / TEXT / ACK lines from the device
    raw_left.wav ...         audio artifacts
    dashboard_export.html    static report
"""
from __future__ import annotations

import csv
import json
import os
import re
import time
from typing import Any, Dict, List, Optional

SUMMARY_FIELDS = [
    "path", "name", "enabled", "scored", "scored_by", "detections", "cooldown_suppressed",
    "peak_x1000", "mean_x1000", "min_x1000", "std_x1000", "last_x1000", "windows",
    "frames", "samples", "rms", "peak_abs", "clips", "inf_mean_us", "inf_max_us",
    "deadline_misses", "errors", "crc", "crc_host", "crc_match", "bf_dl", "bf_dr", "bf_dc",
    "bf_gain", "bf_clips",
]

METRICS_FIELDS = [
    "host_time", "run_id", "replay", "sample_index", "path", "name", "windows", "det",
    "cool", "miss", "last_x1000", "peak_x1000", "mean_x1000", "inf_last_us", "inf_mean_us",
    "inf_max_us", "rms", "peak_abs", "clips",
]

STATS_FIELDS = ["host_time", "run_id", "replay", "sample_index", "stream", "name", "rms", "peak", "clips"]


def safe_name(s: str, limit: int = 40) -> str:
    s = re.sub(r"[^A-Za-z0-9_.-]+", "_", s or "").strip("_")
    return s[:limit] or "run"


def run_dir_name(config: Dict[str, Any], when: Optional[float] = None) -> str:
    ts = time.strftime("%Y-%m-%d_%H%M%S", time.localtime(when or time.time()))
    angle = int(config.get("angle_deg", 0))
    dist = int(config.get("distance_cm", 0))
    label = safe_name(str(config.get("label", "")))
    sign = "m" if angle < 0 else ""
    return f"{ts}_angle{sign}{abs(angle):03d}_distance{dist:03d}cm_{label}"


class RunFolder:
    def __init__(self, root: str, config: Dict[str, Any], when: Optional[float] = None):
        self.root = root
        self.name = run_dir_name(config, when)
        self.path = os.path.join(root, self.name)
        os.makedirs(self.path, exist_ok=True)
        self.metadata: Dict[str, Any] = {
            "run_dir": self.name,
            "created": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(when or time.time())),
            "config": config,
            "device": {},
            "completion": "in_progress",
            "warnings": [],
            "audio_files": [],
            "replays": {},
            "quality": "unknown",
        }
        self._events = open(os.path.join(self.path, "events.jsonl"), "a", encoding="utf-8")
        self._log = open(os.path.join(self.path, "device_serial_log.txt"), "a", encoding="utf-8")
        self._metrics = self._csv("metrics_timeseries.csv", METRICS_FIELDS)
        self._stats = self._csv("audio_stats.csv", STATS_FIELDS)
        self.closed = False
        self.save_metadata()

    def _csv(self, name: str, fields: List[str]):
        f = open(os.path.join(self.path, name), "a", newline="", encoding="utf-8")
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        if f.tell() == 0:
            w.writeheader()
        return f, w

    def file(self, name: str) -> str:
        return os.path.join(self.path, name)

    def save_metadata(self) -> None:
        tmp = self.file("run_metadata.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.metadata, f, indent=2)
        os.replace(tmp, self.file("run_metadata.json"))

    def add_event(self, kind: str, data: Dict[str, Any], replay: bool = False) -> None:
        if self.closed:
            return
        rec = {"host_time": time.time(), "type": kind, "replay": replay, **data}
        self._events.write(json.dumps(rec) + "\n")
        self._events.flush()

    def add_log(self, line: str) -> None:
        if self.closed:
            return
        self._log.write(time.strftime("%H:%M:%S ") + line + "\n")
        self._log.flush()

    def add_metrics(self, row: Dict[str, Any]) -> None:
        if self.closed:
            return
        f, w = self._metrics
        w.writerow({"host_time": round(time.time(), 3), **row})
        f.flush()

    def add_stats(self, row: Dict[str, Any]) -> None:
        if self.closed:
            return
        f, w = self._stats
        w.writerow({"host_time": round(time.time(), 3), **row})
        f.flush()

    def warn(self, text: str) -> None:
        if text not in self.metadata["warnings"]:
            self.metadata["warnings"].append(text)

    def write_summary(self, rows: List[Dict[str, Any]]) -> None:
        with open(self.file("run_summary.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)

    def close(self, completion: str = "complete") -> None:
        if self.closed:
            return
        self.closed = True
        self.metadata["completion"] = completion
        self.metadata["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        self.save_metadata()
        for f in (self._events, self._log, self._metrics[0], self._stats[0]):
            try:
                f.close()
            except Exception:
                pass


def list_runs(root: str) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    if not os.path.isdir(root):
        return out
    for name in sorted(os.listdir(root), reverse=True):
        meta_path = os.path.join(root, name, "run_metadata.json")
        if not os.path.isfile(meta_path):
            continue
        try:
            with open(meta_path, encoding="utf-8") as f:
                meta = json.load(f)
        except Exception:
            continue
        cfg = meta.get("config", {})
        summary = load_summary(os.path.join(root, name))
        det = {r["name"]: r.get("detections") for r in summary}
        out.append({
            "run_dir": name,
            "created": meta.get("created"),
            "label": cfg.get("label"),
            "angle_deg": cfg.get("angle_deg"),
            "distance_cm": cfg.get("distance_cm"),
            "preset": cfg.get("preset"),
            "enabled_paths": [p["name"] for p in cfg.get("paths", []) if p.get("enabled")],
            "threshold_x1000": cfg.get("threshold_x1000"),
            "bf_lr": next((f"{p.get('dl')}/{p.get('dr')}" for p in cfg.get("paths", [])
                           if p.get("name") == "bf_lr"), ""),
            "completion": meta.get("completion"),
            "quality": meta.get("quality"),
            "warnings": len(meta.get("warnings", [])),
            "detections": det,
            "replays": sorted(meta.get("replays", {}).keys()),
        })
    return out


def load_summary(run_path: str) -> List[Dict[str, Any]]:
    p = os.path.join(run_path, "run_summary.csv")
    if not os.path.isfile(p):
        return []
    with open(p, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def load_events(run_path: str) -> List[Dict[str, Any]]:
    p = os.path.join(run_path, "events.jsonl")
    if not os.path.isfile(p):
        return []
    out = []
    with open(p, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return out


def load_metadata(run_path: str) -> Dict[str, Any]:
    with open(os.path.join(run_path, "run_metadata.json"), encoding="utf-8") as f:
        return json.load(f)
