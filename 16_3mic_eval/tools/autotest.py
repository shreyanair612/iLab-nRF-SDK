#!/usr/bin/env python3
"""
Hands-free test loop. You turn the rig; the laptop does everything else.

    python3 tools/autotest.py --session day1 --angles 0,30,60,90,-30,-60,-90 --distance 60

For every angle in the list (repeated --repeat times):

  1. The Mac SAYS out loud "set the rig to 30 degrees, then press button 1".
  2. You turn the platform and press BTN1 on the DK. (With --advance auto the
     Mac counts down --auto-seconds instead and starts the run itself.)
  3. The device starts streaming the three microphones + its beamformer over USB.
  4. The Mac plays the test clip through the speaker (afplay).
  5. The run ends (clip length + margin); the Mac saves the audio, analyses it,
     draws the figures, updates the session report, and says "done".
  6. If the recording was degraded (gaps, no clip found in the audio) it says so
     and repeats that angle once.

Nothing on the laptop needs touching until the list is finished. Ctrl+C stops.

Output: sessions/<session>/runs/<date>_<time>_angle<...>_distance<...>cm_<label>/
        sessions/<session>/session_report.html (updated after every test)

    --simulate   no hardware: synthesises recordings from the clip so the whole
                 host pipeline can be exercised on a laptop alone.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from typing import List, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_run  # noqa: E402
import session_report  # noqa: E402
from wav_writer import StreamWriter, read_wav, write_wav  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 16000


# ------------------------------------------------------------------ small helpers

def say(text: str, enabled: bool = True) -> None:
    print(f"\n*** {text}")
    if enabled and shutil.which("say"):
        subprocess.run(["say", text], check=False)


def parse_angles(spec: str) -> List[int]:
    """'0,30,60' or '-90..90:15'"""
    m = re.fullmatch(r"\s*(-?\d+)\.\.(-?\d+):(\d+)\s*", spec)
    if m:
        a, b, s = int(m[1]), int(m[2]), int(m[3])
        return list(range(a, b + 1, s))
    return [int(x) for x in spec.split(",") if x.strip()]


def safe_name(s: str, limit: int = 40) -> str:
    s = re.sub(r"[^A-Za-z0-9_.-]+", "_", s or "").strip("_")
    return s[:limit] or "run"


def run_dir_name(angle: int, distance: int, label: str, when: Optional[float] = None) -> str:
    ts = time.strftime("%Y-%m-%d_%H%M%S", time.localtime(when or time.time()))
    sign = "m" if angle < 0 else ""
    return f"{ts}_angle{sign}{abs(angle):03d}_distance{distance:03d}cm_{safe_name(label)}"


def clip_duration_s(path: str) -> float:
    pcm, rate, ch = read_wav(path)
    return len(pcm) / (2 * ch) / rate


def play_clip(path: str) -> subprocess.Popen:
    return subprocess.Popen(["afplay", path])


# ------------------------------------------------------------------ saving

def save_recording(run_dir: str, rec, config: dict, extra: dict) -> dict:
    os.makedirs(run_dir, exist_ok=True)
    files = {}
    for name, w in rec.streams.items():
        info = w.write(os.path.join(run_dir, f"{name}.wav"))
        files[name] = info
    meta = {
        "run_dir": os.path.basename(run_dir),
        "created": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "config": config,
        "device_run_start": rec.start,
        "device_run_end": rec.end,
        "audio_files": files,
        "audio_stats": rec.stats,
        "warnings": rec.warnings(),
        "host_duration_s": round(rec.host_ended - rec.host_started, 3),
    }
    meta.update(extra)
    with open(os.path.join(run_dir, "run_metadata.json"), "w") as f:
        json.dump(meta, f, indent=2)
    with open(os.path.join(run_dir, "device_log.txt"), "w") as f:
        f.write("\n".join(rec.log))
    return meta


# ------------------------------------------------------------------ simulation

class FakeRecording:
    """Stands in for device_link.Recording when there is no hardware."""

    def __init__(self, clip: np.ndarray, angle: int, spacing_m: float, seed: int):
        rng = np.random.default_rng(seed)
        pre = int(0.8 * RATE)
        post = int(1.0 * RATE)
        n = pre + len(clip) + post
        # far-field: the right mic hears a source at +angle earlier
        tau = spacing_m / 2 * np.sin(np.radians(angle)) / 343.0 * RATE   # samples, half spacing
        mics = {}
        for name, k in (("raw_left", -1.0), ("raw_center", 0.0), ("raw_right", 1.0)):
            d = k * tau
            x = np.zeros(n)
            src = clip * 0.5
            # fractional delay via FFT phase shift
            spec = np.fft.rfft(src, len(src) * 2)
            f = np.fft.rfftfreq(len(src) * 2, 1.0)
            shifted = np.fft.irfft(spec * np.exp(-2j * np.pi * f * (-d)), len(src) * 2)[: len(src)]
            x[pre: pre + len(src)] += shifted
            # independent self-noise plus a diffuse interferer from the left
            x += rng.standard_normal(n) * 0.002
            mics[name] = x
        # a weak, steady interferer from about -40 degrees (right mic hears it last)
        side = rng.standard_normal(n) * 0.004
        for name, k in (("raw_left", 0), ("raw_center", 2), ("raw_right", 4)):
            mics[name] += np.roll(side, k)
        self.streams = {}
        for name, x in mics.items():
            w = StreamWriter(name, RATE, 1)
            w.add(0, np.clip(x * 32768, -32768, 32767).astype("<i2").tobytes())
            self.streams[name] = w
        self.start = {"run_id": seed, "bf": {"delay": [0, 0, 0], "gain": [32767] * 3, "out_gain": 32767}}
        self.end = {"frames": n // 256, "frames_lost": 0, "capture_dropped": 0, "tx_audio_dropped": 0, "bf_clips": 0}
        self.stats = []
        self.log = ["simulated"]
        self.host_started = time.time()
        self.host_ended = time.time() + n / RATE

    def warnings(self):
        return []


# ------------------------------------------------------------------ main loop

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", required=True, help="name of this measurement session")
    ap.add_argument("--angles", default="0,30,60,90,-30,-60,-90", help="'0,30,60' or '-90..90:15'")
    ap.add_argument("--distance", type=int, default=60, help="speaker distance in cm (metadata)")
    ap.add_argument("--env", default="quiet", help="environment label: quiet / medium / loud ...")
    ap.add_argument("--label", default="", help="free text added to every run name")
    ap.add_argument("--clip", default=os.path.join(PROJECT, "clips", "standard.wav"))
    ap.add_argument("--cues", help="cue CSV for the clip (default: <clip>_cues.csv)")
    ap.add_argument("--spacing-mm", type=float, default=140.0, help="left-to-right mic spacing")
    ap.add_argument("--delays", default="0,0,0", help="device beamformer delays left,center,right")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--advance", choices=["button", "auto"], default="button",
                    help="button: wait for BTN1 on the DK; auto: count down and start by itself")
    ap.add_argument("--auto-seconds", type=int, default=15)
    ap.add_argument("--margin", type=float, default=2.5, help="seconds recorded after the clip ends")
    ap.add_argument("--no-voice", action="store_true")
    ap.add_argument("--port")
    ap.add_argument("--simulate", action="store_true")
    ap.add_argument("--max-retries", type=int, default=1)
    args = ap.parse_args()

    voice = not args.no_voice
    if not os.path.isfile(args.clip):
        sys.exit(f"clip not found: {args.clip}\nMake it with:  python3 tools/make_test_clip.py")
    cues = args.cues or args.clip.replace(".wav", "_cues.csv")
    angles = parse_angles(args.angles)
    delays = [int(x) for x in args.delays.split(",")]
    session_dir = os.path.join(PROJECT, "sessions", safe_name(args.session))
    runs_root = os.path.join(session_dir, "runs")
    os.makedirs(runs_root, exist_ok=True)
    clip_len = clip_duration_s(args.clip)
    duration = int(np.ceil(clip_len + args.margin + 1.0))
    plan = [(rep, a) for rep in range(args.repeat) for a in angles]

    print(f"session {session_dir}\nclip {args.clip} ({clip_len:.1f} s), run duration {duration} s, "
          f"{len(plan)} tests: angles {angles} x{args.repeat}, {args.distance} cm, env {args.env}")

    link = None
    log_f = open(os.path.join(session_dir, "session_log.txt"), "a")
    if not args.simulate:
        import device_link
        link = device_link.DeviceLink.open(args.port, log_file=log_f)
        print(f"device on {link.port}: {link.hello.get('fw', '?')}")
        link.command(f"set_distance {args.distance}")
        link.command(f"set_env {args.env}")
        link.command(f"set_clip {os.path.basename(args.clip)}")
        link.command(f"set_duration {duration}")
        link.command(f"set_delay {delays[0]} {delays[1]} {delays[2]}")
    else:
        clip_pcm, _, _ = read_wav(args.clip)
        clip_x = np.frombuffer(clip_pcm, dtype="<i2").astype(np.float64) / 32768.0

    say(f"{len(plan)} tests. Starting.", voice)
    done = 0
    try:
        for idx, (rep, angle) in enumerate(plan):
            attempts = 0
            while True:
                attempts += 1
                label = args.label or f"{args.env}"
                if args.repeat > 1:
                    label += f"_rep{rep + 1}"
                config = {"angle_deg": angle, "distance_cm": args.distance, "environment": args.env,
                          "label": label, "clip": os.path.basename(args.clip), "duration_s": duration,
                          "bf_delays": delays, "spacing_mm": args.spacing_mm}
                run_dir = os.path.join(runs_root, run_dir_name(angle, args.distance, label))

                if args.simulate:
                    say(f"simulating {angle} degrees", False)
                    rec = FakeRecording(clip_x, angle, args.spacing_mm / 1000.0, seed=idx * 10 + attempts)
                else:
                    link.command(f"set_angle {angle}")
                    link.command(f"set_label {label}")
                    link.drain()
                    if args.advance == "button":
                        say(f"Test {idx + 1} of {len(plan)}. Set the rig to {angle} degrees, then press button 1.", voice)
                        start = link.wait_for_run_start(timeout=None)
                    else:
                        say(f"Test {idx + 1} of {len(plan)}. Set the rig to {angle} degrees. "
                            f"Starting in {args.auto_seconds} seconds.", voice)
                        for left in range(args.auto_seconds, 0, -1):
                            if left in (10, 5, 3, 2, 1):
                                say(str(left), voice)
                            time.sleep(1.0)
                        link.command("start")
                        start = link.wait_for_run_start(timeout=5)
                    player = {}

                    def on_started():
                        time.sleep(0.6)          # let the recording open with real room silence
                        player["p"] = play_clip(args.clip)

                    rec = link.record(start, on_started=on_started, idle_timeout=duration + 10)
                    if "p" in player and player["p"].poll() is None:
                        player["p"].terminate()

                meta = save_recording(run_dir, rec, config,
                                      {"clip_path": os.path.abspath(args.clip), "cues_path": os.path.abspath(cues),
                                       "spacing_mm": args.spacing_mm, "simulated": args.simulate})
                try:
                    result = analyze_run.analyze(run_dir, args.clip, cues, args.spacing_mm)
                except Exception as e:
                    print(f"analysis failed: {e!r}")
                    result = {"warnings": [f"analysis failed: {e!r}"], "paths": {}}
                session_report.build(session_dir)

                bad = [w for w in meta["warnings"] if "gap" in w or "lost" in w or "dropped" in w]
                bad += [w for w in result.get("warnings", []) if "alignment is weak" in w]
                das = result.get("paths", {}).get("das", {})
                print(f"  saved {run_dir}\n  das: snr {das.get('speech_snr_db')} dB, noise {das.get('noise_level_dbfs')} dBFS; "
                      f"heard at {result.get('tdoa', {}).get('angle_from_lr_deg')}")
                for w in meta["warnings"] + result.get("warnings", []):
                    print("  warning:", w)
                if bad and attempts <= args.max_retries and not args.simulate:
                    say("That recording was degraded. Repeating the same angle. Press button 1 when ready.", voice)
                    continue
                done += 1
                remaining = len(plan) - idx - 1
                say(f"Done. {remaining} left." if remaining else "All tests done.", voice)
                break
    except KeyboardInterrupt:
        say("Stopped.", voice)
    finally:
        if link:
            try:
                link.command("stop")
            except Exception:
                pass
            link.close()
        log_f.close()
    ranking = session_report.build(session_dir)
    if ranking:
        print("\nranking so far:")
        for r in ranking:
            print(f"  {r['path']:12s} score {r['score']}  side_rej {r['side_rejection']}  snr0 {r['snr_front']}")
    print(f"\n{done} tests saved. Open {os.path.join(session_dir, 'session_report.html')}")


if __name__ == "__main__":
    main()
