#!/usr/bin/env python3
"""
Compares every test in a session and says which path did best.

    python3 tools/session_report.py sessions/<name>

Reads  runs/*/analysis.json (written by analyze_run.py)
Writes session_report.html plus:
    polar_noise.png        beam pattern: pink-noise level vs angle, per path, normalised to 0°
    snr_vs_angle.png       speech SNR vs angle, per path
    beam_vs_freq_<p>.png   angle x frequency heat map from the sweep, per path
    angle_check.png        angle the microphones heard vs the angle the rig was set to
    ranking.csv            one row per path: the scores below

Scores (all dB, computed on the same recordings):
    front_level      noise level at 0°
    side_rejection   front level minus the mean level at |angle| >= 45°   (higher is better)
    di_estimate      directivity index from the polar                      (higher is better)
    snr_front        speech SNR at 0°                                        (higher is better)
    snr_side_mean    mean speech SNR at |angle| >= 45°                       (LOWER is better:
                                                                             a talker at the side
                                                                             should not get through)
    score            side_rejection + (snr_front - snr_side_mean)
"""
from __future__ import annotations

import argparse
import csv
import glob
import html
import json
import math
import os
import sys
from collections import defaultdict
from typing import Dict, List

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsp_paths as dsp  # noqa: E402

RANK_PATHS = ["raw_center", "raw_left", "raw_right", "lr_mix", "das", "das_c1", "das_c2", "gsc", "diff_lr", "bf_device"]
PLOT_PATHS = ["raw_center", "lr_mix", "das", "das_c1", "gsc", "diff_lr"]


def load_runs(session_dir: str) -> List[dict]:
    runs = []
    for p in sorted(glob.glob(os.path.join(session_dir, "runs", "*", "analysis.json"))):
        try:
            r = json.load(open(p))
        except ValueError:
            continue
        r["_dir"] = os.path.dirname(p)
        if r.get("angle_deg") is not None:
            runs.append(r)
    return runs


def by_angle(runs: List[dict], path: str, key: str) -> Dict[float, float]:
    """Mean of `key` for `path` at each angle (repeats averaged)."""
    acc: Dict[float, List[float]] = defaultdict(list)
    for r in runs:
        v = r["paths"].get(path, {}).get(key)
        if v is not None:
            acc[float(r["angle_deg"])].append(v)
    return {a: float(np.mean(v)) for a, v in acc.items()}


def rank(runs: List[dict]) -> List[dict]:
    rows = []
    for p in RANK_PATHS:
        noise = by_angle(runs, p, "noise_level_dbfs")
        snr = by_angle(runs, p, "speech_snr_db")
        if not noise and not snr:
            continue
        front = noise.get(0.0)
        sides = [v for a, v in noise.items() if abs(a) >= 45]
        side_rej = (front - float(np.mean(sides))) if (front is not None and sides) else None
        snr_front = snr.get(0.0)
        snr_sides = [v for a, v in snr.items() if abs(a) >= 45]
        snr_side = float(np.mean(snr_sides)) if snr_sides else None
        score = None
        if side_rej is not None and snr_front is not None and snr_side is not None:
            score = side_rej + (snr_front - snr_side)
        rows.append({
            "path": p,
            "front_level": None if front is None else round(front, 2),
            "side_rejection": None if side_rej is None else round(side_rej, 2),
            "di_estimate": None if not noise else (lambda d: None if d is None else round(d, 2))(dsp.directivity_index(noise)),
            "snr_front": None if snr_front is None else round(snr_front, 2),
            "snr_side_mean": None if snr_side is None else round(snr_side, 2),
            "angles": len(noise or snr),
            "score": None if score is None else round(score, 2),
        })
    rows.sort(key=lambda r: (r["score"] is None, -(r["score"] or 0)))
    return rows


def figures(session_dir: str, runs: List[dict]) -> List[str]:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    made: List[str] = []
    paths = [p for p in PLOT_PATHS if any(p in r["paths"] for r in runs)]

    # polar beam pattern from the noise burst
    fig = plt.figure(figsize=(7, 7))
    ax = fig.add_subplot(111, projection="polar")
    ax.set_theta_zero_location("N")
    ax.set_theta_direction(-1)
    plotted = False
    for p in paths:
        d = by_angle(runs, p, "noise_level_dbfs")
        if len(d) < 2:
            continue
        ref = d.get(0.0, max(d.values()))
        angs = sorted(d)
        th = np.radians(angs)
        val = [d[a] - ref for a in angs]
        ax.plot(th, val, marker="o", ms=4, label=p)
        plotted = True
    if plotted:
        ax.set_rmin(-30)
        ax.set_rmax(8)
        ax.set_title("beam pattern (pink noise), dB relative to 0°", pad=18)
        ax.legend(loc="lower left", bbox_to_anchor=(-0.15, -0.1), fontsize=8)
        fig.savefig(os.path.join(session_dir, "polar_noise.png"), dpi=110, bbox_inches="tight")
        made.append("polar_noise.png")
    plt.close(fig)

    # SNR vs angle
    fig, ax = plt.subplots(figsize=(9, 4.5))
    plotted = False
    for p in paths:
        d = by_angle(runs, p, "speech_snr_db")
        if len(d) < 2:
            continue
        angs = sorted(d)
        ax.plot(angs, [d[a] for a in angs], marker="o", ms=4, label=p)
        plotted = True
    if plotted:
        ax.set_xlabel("rig angle (°)  — 0° is straight ahead")
        ax.set_ylabel("speech SNR (dB)")
        ax.set_title("speech SNR vs angle (want: high at 0°, low at the sides)")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(os.path.join(session_dir, "snr_vs_angle.png"), dpi=110)
        made.append("snr_vs_angle.png")
    plt.close(fig)

    # beam pattern vs frequency (sweep)
    for p in paths:
        angs = sorted({float(r["angle_deg"]) for r in runs if "sweep_bands_dbfs" in r["paths"].get(p, {})})
        if len(angs) < 2:
            continue
        bands = sorted({int(k) for r in runs for k in r["paths"].get(p, {}).get("sweep_bands_dbfs", {})})
        M = np.full((len(angs), len(bands)), np.nan)
        for i, a in enumerate(angs):
            rs = [r for r in runs if float(r["angle_deg"]) == a and "sweep_bands_dbfs" in r["paths"].get(p, {})]
            for j, b in enumerate(bands):
                vals = [r["paths"][p]["sweep_bands_dbfs"].get(str(b)) for r in rs]
                vals = [v for v in vals if v is not None]
                if vals:
                    M[i, j] = np.mean(vals)
        if 0.0 in angs:
            M = M - M[angs.index(0.0)][None, :]
            label = "dB relative to 0°"
        else:
            label = "dBFS"
        fig, ax = plt.subplots(figsize=(9, 4.5))
        im = ax.imshow(M, aspect="auto", origin="lower", cmap="viridis", vmin=-25, vmax=3,
                       extent=[-0.5, len(bands) - 0.5, -0.5, len(angs) - 0.5])
        ax.set_xticks(range(len(bands)))
        ax.set_xticklabels([str(b) for b in bands], rotation=45, fontsize=8)
        ax.set_yticks(range(len(angs)))
        ax.set_yticklabels([f"{a:+.0f}°" for a in angs], fontsize=8)
        ax.set_xlabel("third-octave band (Hz)")
        ax.set_ylabel("rig angle")
        ax.set_title(f"{p}: beam pattern vs frequency ({label})")
        fig.colorbar(im, ax=ax, label=label)
        fig.tight_layout()
        name = f"beam_vs_freq_{p}.png"
        fig.savefig(os.path.join(session_dir, name), dpi=110)
        made.append(name)
        plt.close(fig)

    # angle check
    pts = [(float(r["angle_deg"]), r["tdoa"]["angle_from_lr_deg"]) for r in runs
           if r.get("tdoa", {}).get("angle_from_lr_deg") is not None]
    if pts:
        fig, ax = plt.subplots(figsize=(5, 5))
        ax.plot([-90, 90], [-90, 90], color="#999", lw=1)
        ax.scatter([a for a, _ in pts], [b for _, b in pts], color="#c0392b")
        ax.set_xlabel("rig angle (°)")
        ax.set_ylabel("angle heard by the mics (°)")
        ax.set_title("angle check: points should sit on the line")
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(session_dir, "angle_check.png"), dpi=110)
        made.append("angle_check.png")
        plt.close(fig)
    return made


def write_html(session_dir: str, runs: List[dict], ranking: List[dict], figs: List[str]):
    h = [f"<h1>{html.escape(os.path.basename(os.path.abspath(session_dir)))}</h1>",
         f"<p>{len(runs)} tests. Angles: {sorted({r['angle_deg'] for r in runs})}. "
         f"Distances: {sorted({r.get('distance_cm') for r in runs})}. "
         f"Environments: {sorted({str(r.get('environment')) for r in runs})}.</p>"]
    if ranking:
        best = ranking[0]
        h.append(f"<p class=best>Best so far: <b>{best['path']}</b> (score {best['score']}). "
                 "See the scoring notes at the bottom.</p>")
        keys = ["path", "score", "side_rejection", "di_estimate", "snr_front", "snr_side_mean", "front_level", "angles"]
        h.append("<table><tr>" + "".join(f"<th>{k}</th>" for k in keys) + "</tr>")
        for r in ranking:
            h.append("<tr>" + "".join(f"<td>{'' if r[k] is None else r[k]}</td>" for k in keys) + "</tr>")
        h.append("</table>")
    for f in figs:
        h.append(f"<h2>{f}</h2><img src='{f}' style='max-width:100%'>")
    h.append("<h2>tests</h2><table><tr><th>run</th><th>angle</th><th>dist</th><th>env</th><th>heard at</th><th>warnings</th></tr>")
    for r in runs:
        rel = os.path.relpath(r["_dir"], session_dir)
        heard = r.get("tdoa", {}).get("angle_from_lr_deg")
        h.append(f"<tr><td><a href='{rel}/report.html'>{html.escape(r['run_dir'])}</a></td><td>{r['angle_deg']}</td>"
                 f"<td>{r.get('distance_cm')}</td><td>{html.escape(str(r.get('environment')))}</td>"
                 f"<td>{'' if heard is None else f'{heard:+.0f}°'}</td>"
                 f"<td class=warn>{html.escape('; '.join(r.get('warnings', [])))}</td></tr>")
    h.append("</table>")
    h.append("<h2>how to read the scores</h2><pre>" + html.escape(__doc__.split("Scores")[1]) + "</pre>")
    css = "<style>body{font-family:-apple-system,Helvetica,sans-serif;margin:24px}table{border-collapse:collapse}" \
          "td,th{border:1px solid #ccc;padding:3px 8px;font-size:13px}.warn{color:#a33}.best{font-size:1.1em}</style>"
    with open(os.path.join(session_dir, "session_report.html"), "w") as f:
        f.write("<!doctype html><meta charset=utf-8>" + css + "\n".join(h))


def build(session_dir: str) -> List[dict]:
    runs = load_runs(session_dir)
    ranking = rank(runs)
    figs = figures(session_dir, runs) if runs else []
    with open(os.path.join(session_dir, "ranking.csv"), "w", newline="") as f:
        if ranking:
            w = csv.DictWriter(f, fieldnames=list(ranking[0].keys()))
            w.writeheader()
            w.writerows(ranking)
    write_html(session_dir, runs, ranking, figs)
    return ranking


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session_dir")
    args = ap.parse_args()
    ranking = build(args.session_dir)
    for r in ranking:
        print(f"{r['path']:12s} score {r['score']}  side_rej {r['side_rejection']}  DI {r['di_estimate']}  "
              f"snr0 {r['snr_front']}  snr_side {r['snr_side_mean']}")
    print("report:", os.path.join(args.session_dir, "session_report.html"))


if __name__ == "__main__":
    main()
