#!/usr/bin/env python3
"""
Applies the pass criteria of TEST_PLAN.md to recorded sessions and prints
PASS / FAIL per claim with bootstrap 95 % intervals over repeats.

    python3 tools/verdict.py sessions/s1_integrity_sweep sessions/s2_fit_centred ...

Which claim a session is judged against comes from its name:
    integrity|sweep  -> claims 1 and 2
    fit              -> claim 3 (centred / shimL / shimR / distract from the name)
    safety           -> claim 4
    twospk           -> claim 5
or force it with --claim N. The thresholds live in CRITERIA below and are the
ones written in TEST_PLAN.md; change them there first.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from typing import Callable, Dict, List, Optional, Tuple

import numpy as np

CRITERIA = {
    "tdoa_tolerance_deg": 10.0,
    "lr_match_db": 1.0,
    "no_speech_loss_db": 0.5,
    "fit_centre_rate": 0.9,
    "fit_centre_delay_rate": 0.8,
    "fit_gain_db": 1.0,
    "fit_tilt_rate": 0.8,
    "fit_distract_rate": 0.8,
    "fit_neutral_db": 0.3,
    "preservation_min": 0.98,
    "interf_rejection_lf_db": 6.0,
    "interf_rejection_lf_ci_db": 3.0,
    "snr_under_interf_db": 3.0,
    "within_bound_db": 3.0,
}

GREEN, RED, DIM, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[0m"
if not sys.stdout.isatty():
    GREEN = RED = DIM = RESET = ""


# ------------------------------------------------------------------ helpers

def load(session_dir: str) -> List[dict]:
    runs = []
    for p in sorted(glob.glob(os.path.join(session_dir, "runs", "*", "analysis.json"))):
        try:
            r = json.load(open(p))
            meta_p = os.path.join(os.path.dirname(p), "run_metadata.json")
            r["_meta"] = json.load(open(meta_p)) if os.path.isfile(meta_p) else {}
            runs.append(r)
        except ValueError:
            pass
    return runs


def boot_ci(values: List[float], n: int = 2000, seed: int = 1) -> Tuple[float, float, float]:
    v = np.array([x for x in values if x is not None], dtype=float)
    if v.size == 0:
        return (float("nan"),) * 3
    if v.size == 1:
        return float(v[0]), float(v[0]), float(v[0])
    rng = np.random.default_rng(seed)
    means = [rng.choice(v, v.size, replace=True).mean() for _ in range(n)]
    return float(v.mean()), float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))


def metric(r: dict, path: str, key: str) -> Optional[float]:
    return r.get("paths", {}).get(path, {}).get(key)


def diff(runs: List[dict], a: str, b: str, key: str) -> List[float]:
    out = []
    for r in runs:
        x, y = metric(r, a, key), metric(r, b, key)
        if x is not None and y is not None:
            out.append(x - y)
    return out


class Report:
    def __init__(self):
        self.lines: List[str] = []
        self.failed = 0
        self.passed = 0

    def check(self, name: str, ok: Optional[bool], detail: str):
        if ok is None:
            tag = f"{DIM}  n/a {RESET}"
        elif ok:
            tag = f"{GREEN} PASS {RESET}"
            self.passed += 1
        else:
            tag = f"{RED} FAIL {RESET}"
            self.failed += 1
        self.lines.append(f"  [{tag}] {name:48s} {detail}")

    def ci_check(self, name: str, values: List[float], lower_bound: float, want_mean: Optional[float] = None):
        """PASS when the CI's lower bound clears `lower_bound` (and the mean clears want_mean)."""
        m, lo, hi = boot_ci(values)
        if np.isnan(m):
            self.check(name, None, "no data")
            return
        ok = lo >= lower_bound and (want_mean is None or m >= want_mean)
        self.check(name, ok, f"mean {m:+.2f}  95% CI [{lo:+.2f}, {hi:+.2f}]  n={len(values)}  need CI low >= {lower_bound:+.2f}"
                   + (f", mean >= {want_mean:+.2f}" if want_mean is not None else ""))

    def rate_check(self, name: str, hits: List[bool], min_rate: float):
        if not hits:
            self.check(name, None, "no data")
            return
        rate = sum(hits) / len(hits)
        self.check(name, rate >= min_rate, f"{sum(hits)}/{len(hits)} = {rate:.0%}  need >= {min_rate:.0%}")


# ------------------------------------------------------------------ claims

def claim_1(runs: List[dict], rep: Report):
    rep.lines.append("Claim 1 - capture integrity")
    lost = [r["_meta"].get("device_run_end", {}).get("frames_lost", 0) or 0 for r in runs]
    gaps = [sum(len(f.get("gaps", [])) for f in r["_meta"].get("audio_files", {}).values()) for r in runs]
    rep.check("no audio lost (frames_lost == 0, no WAV gaps)", all(x == 0 for x in lost) and all(g == 0 for g in gaps),
              f"{sum(1 for x in lost if x)} runs with lost frames, {sum(1 for g in gaps if g)} with gaps, of {len(runs)}")
    errs = []
    for r in runs:
        a = r.get("angle_deg")
        h = r.get("tdoa", {}).get("angle_from_lr_deg")
        if a is not None and h is not None and abs(a) <= 60:
            errs.append(abs(h - a))
    rep.check(f"mics hear the rig angle (|err| <= {CRITERIA['tdoa_tolerance_deg']:.0f} deg, |angle| <= 60)",
              bool(errs) and max(errs) <= CRITERIA["tdoa_tolerance_deg"],
              f"max error {max(errs):.1f} deg over {len(errs)} runs" if errs else "no data")
    mism = [abs((metric(r, "raw_left", "noise_level_dbfs") or 0) - (metric(r, "raw_right", "noise_level_dbfs") or 0))
            for r in runs if r.get("angle_deg") == 0 and metric(r, "raw_left", "noise_level_dbfs") is not None]
    rep.check(f"left/right level match at 0 deg (<= {CRITERIA['lr_match_db']} dB)",
              bool(mism) and max(mism) <= CRITERIA["lr_match_db"],
              f"max mismatch {max(mism):.2f} dB over {len(mism)} runs" if mism else "no 0 deg runs")
    dev = [r.get("device_bf_matches_host") for r in runs if "device_bf_matches_host" in r]
    rep.check("device beamformer == host reconstruction", all(dev) if dev else None,
              f"{sum(1 for d in dev if d)}/{len(dev)} runs" if dev else "no device stream recorded (simulation?)")


def claim_2(runs: List[dict], rep: Report):
    rep.lines.append("Claim 2 - delay-and-sum beats one mic without hurting the wearer")

    def side_rej(path: str) -> List[float]:
        # per repeat: level at 0 minus mean level at |angle| >= 45, from the sweep bands 1-4 kHz
        out = []
        reps = sorted({r["_meta"].get("config", {}).get("label", "") for r in runs})
        for lab in reps:
            rs = [r for r in runs if r["_meta"].get("config", {}).get("label", "") == lab]
            front = [r for r in rs if r.get("angle_deg") == 0]
            sides = [r for r in rs if abs(r.get("angle_deg", 0)) >= 45]
            if not front or not sides:
                continue
            bands = ("1000", "1250", "1600", "2000", "2500", "3150", "4000")
            f = np.mean([front[0]["paths"][path]["sweep_bands_dbfs"][b] for b in bands if b in front[0]["paths"].get(path, {}).get("sweep_bands_dbfs", {})])
            s_ = np.mean([r["paths"][path]["sweep_bands_dbfs"][b] for r in sides for b in bands if b in r["paths"].get(path, {}).get("sweep_bands_dbfs", {})])
            out.append(float(f - s_))
        return out

    d = [a - b for a, b in zip(side_rej("das"), side_rej("raw_center"))]
    rep.ci_check("side rejection: das - raw_center > 0 (1-4 kHz)", d, 0.0)
    rep.ci_check(f"no speech loss at 0 deg: snr(das) - snr(raw_center) >= -{CRITERIA['no_speech_loss_db']}",
                 diff([r for r in runs if r.get("angle_deg") == 0], "das", "raw_center", "speech_snr_db"),
                 -CRITERIA["no_speech_loss_db"])


def claim_3(runs: List[dict], rep: Report, variant: str):
    rep.lines.append(f"Claim 3 - per-wearer fit ({variant})")
    fits = [r.get("wearer_fit", {}) for r in runs]
    valid = [f for f in fits if f.get("valid")]
    if variant == "centred":
        rep.rate_check(f"finds centre (lr_lag == 0)", [f.get("lr_lag") == 0 for f in valid], CRITERIA["fit_centre_rate"])
        rep.rate_check("centre delay == 1", [f.get("delay_center") == 1 for f in valid], CRITERIA["fit_centre_delay_rate"])
        g = [20 * np.log10(f["gain_right"]) for f in valid if f.get("gain_right")]
        rep.check(f"R gain trim within +-{CRITERIA['fit_gain_db']} dB", bool(g) and max(abs(x) for x in g) <= CRITERIA["fit_gain_db"],
                  f"max |trim| {max(abs(x) for x in g):.2f} dB" if g else "no data")
        rep.ci_check(f"fit neutral: snr(das_auto) - snr(das) >= -{CRITERIA['fit_neutral_db']}",
                     diff(runs, "das_auto", "das", "speech_snr_db"), -CRITERIA["fit_neutral_db"])
    elif variant in ("shimL", "shimR"):
        # shim under the LEFT temple lifts the left mic away from the mouth: left hears later -> lag < 0
        want = -1 if variant == "shimL" else 1
        rep.rate_check(f"finds the tilt (sign of lr_lag == {want:+d}, |lag| >= 1)",
                       [np.sign(f.get("lr_lag", 0)) == want for f in valid], CRITERIA["fit_tilt_rate"])
        rep.ci_check("fit helps: snr(das_auto) - snr(das) > 0", diff(runs, "das_auto", "das", "speech_snr_db"), 0.0)
    elif variant == "distract":
        rep.rate_check("not fooled (lr_lag == 0, or fit rejected)",
                       [(not f.get("valid")) or f.get("lr_lag") == 0 for f in fits], CRITERIA["fit_distract_rate"])
    rep.check("fits attempted", bool(fits), f"{len(valid)} valid of {len(fits)}")


def claim_4(runs: List[dict], rep: Report):
    rep.lines.append("Claim 4 - the canceller never harms the wearer")
    for path in ("gsc_nlms", "gsc_auto"):
        pres = [metric(r, path, "speech_preservation") for r in runs]
        pres = [p for p in pres if p is not None]
        rep.check(f"{path}: speech preserved (corr with das >= {CRITERIA['preservation_min']}) in every run",
                  bool(pres) and min(pres) >= CRITERIA["preservation_min"],
                  f"min {min(pres):.4f} over {len(pres)} runs" if pres else "no data")
        rep.ci_check(f"{path}: no SNR loss, snr - snr(das) >= -{CRITERIA['no_speech_loss_db']}",
                     diff(runs, path, "das", "speech_snr_db"), -CRITERIA["no_speech_loss_db"])
        clips = [metric(r, path, "clips") or 0 for r in runs]
        rep.check(f"{path}: no clipping", all(c == 0 for c in clips), f"{sum(clips)} clipped samples")


def claim_5(runs: List[dict], rep: Report):
    rep.lines.append("Claim 5 - the canceller helps against a second talker")
    angles = sorted({r.get("angle_deg") for r in runs})
    for path in ("gsc_nlms", "gsc_auto"):
        for a in angles:
            rs = [r for r in runs if r.get("angle_deg") == a]
            rep.ci_check(f"{path} @ interferer {a:+d}: LF rejection - das >= {CRITERIA['interf_rejection_lf_db']} dB",
                         diff(rs, path, "das", "interf_rejection_lf_db"),
                         CRITERIA["interf_rejection_lf_ci_db"], CRITERIA["interf_rejection_lf_db"])
        rep.ci_check(f"{path}: wearer SNR under interference - das >= {CRITERIA['snr_under_interf_db']} dB",
                     diff(runs, path, "das", "snr_under_interference_db"), 0.0, CRITERIA["snr_under_interf_db"])
        gap = diff(runs, "gsc", path, "interf_rejection_lf_db")
        m, lo, hi = boot_ci(gap)
        rep.check(f"{path}: within {CRITERIA['within_bound_db']} dB of the offline optimum (gsc)",
                  (not np.isnan(m)) and m <= CRITERIA["within_bound_db"], f"optimum - {path} = {m:+.2f} dB" if not np.isnan(m) else "no data")
    rep.check("wake-word replay (14_ww_eval) on gsc_nlms vs das", None, "run by hand: see TEST_PLAN.md claim 5")


def classify(name: str, forced: Optional[int]) -> List[Tuple[int, str]]:
    if forced:
        return [(forced, "")]
    n = name.lower()
    out = []
    if re.search(r"integrity|sweep", n):
        out += [(1, ""), (2, "")]
    if "fit" in n:
        v = "centred"
        for k in ("shiml", "shimr", "distract"):
            if k in n:
                v = {"shiml": "shimL", "shimr": "shimR", "distract": "distract"}[k]
        out.append((3, v))
    if "safety" in n:
        out.append((4, ""))
    if "twospk" in n or "two_speaker" in n:
        out.append((5, ""))
    return out or [(1, "")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sessions", nargs="+")
    ap.add_argument("--claim", type=int, choices=[1, 2, 3, 4, 5])
    ap.add_argument("--variant", choices=["centred", "shimL", "shimR", "distract"])
    args = ap.parse_args()

    total = Report()
    for sd in args.sessions:
        runs = load(sd)
        rep = Report()
        print(f"\n=== {sd}  ({len(runs)} runs)")
        if not runs:
            print("  no analysed runs")
            continue
        for claim, variant in classify(os.path.basename(os.path.normpath(sd)), args.claim):
            if claim == 1:
                claim_1(runs, rep)
            elif claim == 2:
                claim_2(runs, rep)
            elif claim == 3:
                claim_3(runs, rep, args.variant or variant or "centred")
            elif claim == 4:
                claim_4(runs, rep)
            elif claim == 5:
                claim_5(runs, rep)
        print("\n".join(rep.lines))
        total.passed += rep.passed
        total.failed += rep.failed
        with open(os.path.join(sd, "verdict.txt"), "w") as f:
            f.write(re.sub(r"\033\[[0-9;]*m", "", "\n".join(rep.lines)) + "\n")
    print(f"\n{total.passed} passed, {total.failed} failed")
    sys.exit(1 if total.failed else 0)


if __name__ == "__main__":
    main()
