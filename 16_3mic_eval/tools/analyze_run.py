#!/usr/bin/env python3
"""
Analyses one recorded test: builds every beamformer path from the three raw
microphones, lines the played clip up with the recording, measures each path
per clip segment, and draws the pictures.

    python3 tools/analyze_run.py sessions/<name>/runs/<run_dir>
    python3 tools/analyze_run.py <run_dir> --clip clips/standard.wav --spacing-mm 140

Reads   raw_left.wav raw_center.wav raw_right.wav [bf_device.wav] run_metadata.json
Writes  paths/<path>.wav          every derived path as audio you can listen to
        analysis.json             all numbers (levels per segment, SNR, bands, TDOA)
        fig_waveforms.png         all paths stacked, clip segments shaded
        fig_spectrograms.png      raw center vs the beamformers
        fig_levels.png            speech SNR and noise-burst level per path
        fig_response.png          third-octave response of each path during the sweep
        report.html               the figures and a table, for a browser

autotest.py calls this after every test; run it by hand to re-analyse old
recordings with new DSP (everything here is recomputed from the raw files).
"""
from __future__ import annotations

import argparse
import csv
import html
import json
import os
import sys
from typing import Dict, List, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsp_paths as dsp  # noqa: E402
from wav_writer import read_wav, write_wav  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Paths shown in the spectrogram grid and the response plot, in this order.
FOCUS_PATHS = ["raw_center", "lr_mix", "das", "das_c1", "gsc", "diff_lr"]
SEG_COLORS = {"silence": "#eeeeee", "noise": "#ffe8c2", "speech": "#cfe8ff", "sweep": "#e2ffd6", "tone": "#f3d6ff"}


def load_mono(path: str) -> np.ndarray:
    pcm, rate, ch = read_wav(path)
    x = np.frombuffer(pcm, dtype="<i2")
    if ch != 1:
        x = x.reshape(-1, ch)[:, 0]
    if rate != dsp.RATE:
        raise ValueError(f"{path}: expected {dsp.RATE} Hz, got {rate}")
    return x.copy()


def load_cues(path: str) -> List[dsp.Segment]:
    segs: List[dsp.Segment] = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            segs.append(dsp.Segment(row["name"], row["kind"], float(row["start_s"]), float(row["end_s"]),
                                    row.get("text", "")))
    return segs


def find_clip(run_dir: str, meta: dict, override: Optional[str]) -> Optional[str]:
    cands = [override, meta.get("clip_path"), meta.get("config", {}).get("clip")]
    for c in cands:
        if not c:
            continue
        for p in (c, os.path.join(PROJECT, "clips", c), os.path.join(PROJECT, "clips", c + ".wav"),
                  os.path.join(run_dir, c)):
            if os.path.isfile(p):
                return p
    return None


def analyze(run_dir: str, clip_path: Optional[str] = None, cues_path: Optional[str] = None,
            spacing_mm: Optional[float] = None, make_figures: bool = True) -> dict:
    meta_path = os.path.join(run_dir, "run_metadata.json")
    meta = json.load(open(meta_path)) if os.path.isfile(meta_path) else {}
    cfg = meta.get("config", {})
    spacing_mm = spacing_mm or meta.get("spacing_mm") or 140.0

    L = load_mono(os.path.join(run_dir, "raw_left.wav"))
    C = load_mono(os.path.join(run_dir, "raw_center.wav"))
    R = load_mono(os.path.join(run_dir, "raw_right.wav"))
    bf_dev = None
    if os.path.isfile(os.path.join(run_dir, "bf_device.wav")):
        bf_dev = load_mono(os.path.join(run_dir, "bf_device.wav"))

    paths = dsp.build_paths(L, C, R, bf_dev)
    n = len(paths["raw_center"])

    result: dict = {
        "run_dir": os.path.basename(os.path.abspath(run_dir)),
        "angle_deg": cfg.get("angle_deg"),
        "distance_cm": cfg.get("distance_cm"),
        "environment": cfg.get("environment"),
        "label": cfg.get("label"),
        "duration_s": round(n / dsp.RATE, 3),
        "spacing_mm": spacing_mm,
        "paths": {},
        "warnings": list(meta.get("warnings", [])),
    }

    # Device beamformer check: the host's `das` with the device's delays must match bit for bit.
    dev_bf_cfg = meta.get("device_run_start", {}).get("bf")
    if bf_dev is not None and dev_bf_cfg:
        host = dsp.delay_and_sum_int((L, C, R), dev_bf_cfg["delay"], dev_bf_cfg["gain"], dev_bf_cfg["out_gain"])
        m = min(len(host), len(bf_dev))
        # the device's delay lines start empty at run start; skip the first frame
        match = bool(np.array_equal(host[256:m], bf_dev[256:m]))
        result["device_bf_matches_host"] = match
        if not match:
            diff = int(np.max(np.abs(host[256:m].astype(np.int32) - bf_dev[256:m].astype(np.int32))))
            result["warnings"].append(f"device beamformer differs from host reconstruction (max |diff| {diff})")

    # Clip alignment and segments.
    clip = find_clip(run_dir, meta, clip_path)
    segments: List[dsp.Segment] = []
    offset = 0
    if clip:
        cues = cues_path or meta.get("cues_path") or clip.replace(".wav", "_cues.csv")
        if os.path.isfile(cues):
            segments = load_cues(cues)
        clip_x = load_mono(clip)
        offset, conf = dsp.align_clip(C, clip_x)
        result["clip"] = os.path.basename(clip)
        result["clip_offset_samples"] = int(offset)
        result["clip_offset_s"] = round(offset / dsp.RATE, 3)
        result["clip_align_confidence"] = round(conf, 3)
        if conf < 0.2:
            result["warnings"].append(f"clip alignment is weak (confidence {conf:.2f}); was the speaker playing?")
    else:
        result["warnings"].append("no clip found for this run; per-segment measurements skipped")

    speech_segs = [s.slice(offset, n) for s in segments if s.kind == "speech"]
    silence_segs = [s.slice(offset, n) for s in segments if s.kind == "silence"]
    noise_segs = [s.slice(offset, n) for s in segments if s.kind == "noise"]
    sweep_segs = [s.slice(offset, n) for s in segments if s.kind == "sweep"]
    seg_levels = dsp.segment_levels(paths, segments, offset) if segments else {}

    for name, x in paths.items():
        row: dict = {
            "rms_dbfs": round(dsp.dbfs(dsp.rms(dsp.to_float(x))), 2),
            "peak": int(np.max(np.abs(x.astype(np.int32)))) if len(x) else 0,
            "clips": int(np.sum((x == 32767) | (x == -32768))),
            "segments": {k: round(v, 2) for k, v in seg_levels.get(name, {}).items()},
        }
        if speech_segs and silence_segs:
            snr = dsp.speech_snr(x, speech_segs, silence_segs)
            row["speech_snr_db"] = None if snr is None else round(snr, 2)
        if noise_segs:
            a, b = noise_segs[0]
            t = dsp.RATE // 10
            row["noise_level_dbfs"] = round(dsp.dbfs(dsp.rms(dsp.to_float(x[a + t:b - t]))), 2)
        if silence_segs:
            floor = [dsp.rms(dsp.to_float(x[a:b])) for a, b in silence_segs if b - a > dsp.RATE // 4]
            if floor:
                row["silence_floor_dbfs"] = round(dsp.dbfs(float(np.median(floor))), 2)
        if sweep_segs:
            a, b = sweep_segs[0]
            row["sweep_bands_dbfs"] = {str(k): round(v, 2) for k, v in dsp.band_levels(x[a:b]).items()}
        result["paths"][name] = row

    # Time difference of arrival on the noise burst: checks the rig angle and the slot order.
    if noise_segs:
        a, b = noise_segs[0]
        d_lr, peak_lr = dsp.gcc_phat(L[a:b], R[a:b])
        d_lc, peak_lc = dsp.gcc_phat(L[a:b], C[a:b])
        d_cr, peak_cr = dsp.gcc_phat(C[a:b], R[a:b])
        result["tdoa"] = {
            "left_right_samples": round(d_lr, 2), "left_right_peak": round(peak_lr, 1),
            "left_center_samples": round(d_lc, 2), "center_right_samples": round(d_cr, 2),
            "angle_from_lr_deg": dsp.tdoa_angle(d_lr, spacing_mm / 1000.0),
            "consistency_samples": round(d_lr - (d_lc + d_cr), 2),
        }
        if result["tdoa"]["angle_from_lr_deg"] is not None and cfg.get("angle_deg") is not None:
            est = result["tdoa"]["angle_from_lr_deg"]
            if abs(est - float(cfg["angle_deg"])) > 20:
                result["warnings"].append(
                    f"microphones hear the source at {est:+.0f} deg but the rig says {cfg['angle_deg']:+d} deg: "
                    "check the angle marking, the slot order (left/right swapped?) and --spacing-mm")

    # Derived audio for listening.
    pdir = os.path.join(run_dir, "paths")
    os.makedirs(pdir, exist_ok=True)
    for name, x in paths.items():
        if name.startswith("raw_") or name == "bf_device":
            continue
        write_wav(os.path.join(pdir, f"{name}.wav"), x.astype("<i2").tobytes(), dsp.RATE, 1)

    if make_figures:
        try:
            figures(run_dir, paths, segments, offset, result)
        except Exception as e:  # figures must never block the test loop
            result["warnings"].append(f"figures failed: {e!r}")

    with open(os.path.join(run_dir, "analysis.json"), "w") as f:
        json.dump(result, f, indent=2)
    write_report(run_dir, result)
    return result


# ------------------------------------------------------------------ figures

def _shade(ax, segments: List[dsp.Segment], offset: int):
    for s in segments:
        a = offset / dsp.RATE + s.start_s
        b = offset / dsp.RATE + s.end_s
        ax.axvspan(a, b, color=SEG_COLORS.get(s.kind, "#eee"), alpha=0.6, lw=0)


def figures(run_dir: str, paths: Dict[str, np.ndarray], segments: List[dsp.Segment], offset: int, result: dict):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    title = f"{result.get('label') or ''}  angle {result.get('angle_deg')}°  {result.get('distance_cm')} cm  {result.get('environment') or ''}"

    # 1. waveforms
    names = [p for p in ["raw_left", "raw_center", "raw_right", "lr_mix", "das", "das_c1", "gsc", "diff_lr", "bf_device"] if p in paths]
    fig, axes = plt.subplots(len(names), 1, figsize=(14, 1.4 * len(names) + 1), sharex=True)
    for ax, name in zip(np.atleast_1d(axes), names):
        x = paths[name]
        t = np.arange(len(x)) / dsp.RATE
        step = max(1, len(x) // 40000)
        _shade(ax, segments, offset)
        ax.plot(t[::step], x[::step] / 32768.0, lw=0.5, color="#1f4e79")
        ax.set_ylim(-1, 1)
        ax.set_ylabel(name, rotation=0, ha="right", va="center", fontsize=9)
        ax.set_yticks([])
        ax.grid(axis="x", alpha=0.3)
    axes[-1].set_xlabel("time (s)")
    fig.suptitle("waveforms: " + title)
    fig.tight_layout()
    fig.savefig(os.path.join(run_dir, "fig_waveforms.png"), dpi=110)
    plt.close(fig)

    # 2. spectrograms
    focus = [p for p in FOCUS_PATHS if p in paths]
    fig, axes = plt.subplots(len(focus), 1, figsize=(14, 2.2 * len(focus) + 1), sharex=True)
    for ax, name in zip(np.atleast_1d(axes), focus):
        S, f, t = dsp.spectrogram(paths[name])
        ax.imshow(S, origin="lower", aspect="auto", extent=[t[0], t[-1] if len(t) > 1 else 1, f[0], f[-1]],
                  vmin=-90, vmax=-10, cmap="magma")
        ax.set_ylabel(f"{name}\nHz", fontsize=9)
        ax.set_ylim(0, 8000)
    axes[-1].set_xlabel("time (s)")
    fig.suptitle("spectrograms: " + title)
    fig.tight_layout()
    fig.savefig(os.path.join(run_dir, "fig_spectrograms.png"), dpi=110)
    plt.close(fig)

    # 3. levels per path
    rows = result["paths"]
    order = [p for p in ["raw_left", "raw_center", "raw_right", "lr_mix", "das", "das_c1", "das_c2", "gsc", "diff_lr", "bf_device"] if p in rows]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4))
    for ax, key, label in zip(axes, ["speech_snr_db", "noise_level_dbfs", "silence_floor_dbfs"],
                              ["speech SNR (dB, higher is better)", "pink-noise burst level (dBFS)", "silence floor (dBFS, lower is better)"]):
        vals = [rows[p].get(key) for p in order]
        ax.bar(range(len(order)), [v if v is not None else 0 for v in vals], color="#2a7fbf")
        for i, v in enumerate(vals):
            if v is not None:
                ax.text(i, v, f"{v:.1f}", ha="center", va="bottom" if v >= 0 else "top", fontsize=8)
        ax.set_xticks(range(len(order)))
        ax.set_xticklabels(order, rotation=45, ha="right", fontsize=8)
        ax.set_title(label, fontsize=10)
        ax.grid(axis="y", alpha=0.3)
    fig.suptitle("levels: " + title)
    fig.tight_layout()
    fig.savefig(os.path.join(run_dir, "fig_levels.png"), dpi=110)
    plt.close(fig)

    # 4. sweep response
    if any("sweep_bands_dbfs" in rows[p] for p in rows):
        fig, ax = plt.subplots(figsize=(10, 4.5))
        for name in [p for p in ["raw_center", "lr_mix", "das", "das_c1", "gsc", "diff_lr"] if p in rows and "sweep_bands_dbfs" in rows[p]]:
            b = rows[name]["sweep_bands_dbfs"]
            fs = sorted(int(k) for k in b)
            ax.semilogx(fs, [b[str(k)] for k in fs], marker="o", ms=3, label=name)
        ax.set_xlabel("third-octave band (Hz)")
        ax.set_ylabel("level during sweep (dBFS)")
        ax.set_title("frequency response at this angle: " + title, fontsize=10)
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(os.path.join(run_dir, "fig_response.png"), dpi=110)
        plt.close(fig)


def write_report(run_dir: str, result: dict):
    rows = result["paths"]
    keys = ["rms_dbfs", "speech_snr_db", "noise_level_dbfs", "silence_floor_dbfs", "peak", "clips"]
    h = [f"<h1>{html.escape(result['run_dir'])}</h1>",
         f"<p>angle {result.get('angle_deg')}°, {result.get('distance_cm')} cm, {html.escape(str(result.get('environment')))}, "
         f"{result.get('duration_s')} s. Clip offset {result.get('clip_offset_s', '?')} s "
         f"(confidence {result.get('clip_align_confidence', '?')}).</p>"]
    if result.get("warnings"):
        h.append("<ul class=warn>" + "".join(f"<li>{html.escape(w)}</li>" for w in result["warnings"]) + "</ul>")
    if "tdoa" in result:
        t = result["tdoa"]
        ang = t.get("angle_from_lr_deg")
        h.append(f"<p>Microphones hear the source at <b>{'n/a' if ang is None else f'{ang:+.0f}°'}</b> "
                 f"(L-R delay {t['left_right_samples']} samples, peak {t['left_right_peak']}).</p>")
    h.append("<table><tr><th>path</th>" + "".join(f"<th>{k}</th>" for k in keys) + "</tr>")
    for p, r in rows.items():
        h.append(f"<tr><td>{p}</td>" + "".join(f"<td>{r.get(k, '')}</td>" for k in keys) + "</tr>")
    h.append("</table>")
    for fig in ["fig_levels.png", "fig_response.png", "fig_waveforms.png", "fig_spectrograms.png"]:
        if os.path.isfile(os.path.join(run_dir, fig)):
            h.append(f"<h2>{fig}</h2><img src='{fig}' style='max-width:100%'>")
    h.append("<h2>listen</h2><ul>")
    for p in sorted(rows):
        f = f"{p}.wav" if p.startswith("raw_") or p == "bf_device" else f"paths/{p}.wav"
        h.append(f"<li>{p}: <audio controls preload=none src='{f}'></audio></li>")
    h.append("</ul>")
    css = "<style>body{font-family:-apple-system,Helvetica,sans-serif;margin:24px}table{border-collapse:collapse}" \
          "td,th{border:1px solid #ccc;padding:3px 8px;font-size:13px}.warn li{color:#a33}</style>"
    with open(os.path.join(run_dir, "report.html"), "w") as f:
        f.write("<!doctype html><meta charset=utf-8>" + css + "\n".join(h))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--clip")
    ap.add_argument("--cues")
    ap.add_argument("--spacing-mm", type=float, help="left-to-right microphone spacing")
    ap.add_argument("--no-figures", action="store_true")
    args = ap.parse_args()
    r = analyze(args.run_dir, args.clip, args.cues, args.spacing_mm, not args.no_figures)
    for p, row in r["paths"].items():
        print(f"{p:12s} rms {row['rms_dbfs']:7.2f} dBFS  snr {row.get('speech_snr_db')}  noise {row.get('noise_level_dbfs')}")
    for w in r["warnings"]:
        print("warning:", w)
    print("report:", os.path.join(args.run_dir, "report.html"))


if __name__ == "__main__":
    main()
