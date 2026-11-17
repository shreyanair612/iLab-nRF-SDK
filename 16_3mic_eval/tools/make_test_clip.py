#!/usr/bin/env python3
"""
Builds the clip the speaker plays during every test, and the cue sheet that
tells the analyzer what is where.

    python3 tools/make_test_clip.py                      # clips/standard.wav + standard_cues.csv
    python3 tools/make_test_clip.py --speech-dir ../14_ww_eval/test_samples/hey_vision --name people
    python3 tools/make_test_clip.py --speech-dir rec/wearer --interferer-dir rec/other --name twospk

With --interferer-dir the clip is STEREO: left = wearer speaker, right =
interferer speaker (a 3.5 mm splitter feeds two speakers). Segment kinds are
then speech (wearer only), interf (interferer only), both, noise (pink on
both), sweep (wearer) and silence, so the analyzer can measure how much
interferer each path removes while the wearer is preserved.

Timeline (16 kHz mono, every part normalized to the same loudness):

    1.0 s  silence          room / self-noise floor
    3.0 s  pink noise       broadband level -> beam pattern per angle
    1.0 s  silence
    N x    speech clips     real "Hey Vision" recordings (or the Mac's voice) with 0.7 s gaps
    1.0 s  silence
    4.0 s  log sine sweep   200 Hz -> 7 kHz: frequency response of each path at this angle
    1.0 s  silence

The cue sheet is a CSV: name,kind,start_s,end_s,text.
"""
from __future__ import annotations

import argparse
import csv
import glob
import math
import os
import subprocess
import sys
import tempfile
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is missing. Install it with:  pip3 install numpy")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wav_writer import read_wav, write_wav  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 16000
TARGET_RMS = 0.08          # about -22 dBFS; leaves headroom for the sweep peaks
PEAK_LIMIT = 0.6


def silence(seconds: float) -> np.ndarray:
    return np.zeros(int(seconds * RATE))


def pink_noise(seconds: float, seed: int = 7) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = int(seconds * RATE)
    white = rng.standard_normal(n)
    spec = np.fft.rfft(white)
    f = np.fft.rfftfreq(n, 1 / RATE)
    f[0] = f[1]
    spec /= np.sqrt(f)
    x = np.fft.irfft(spec, n)
    return fade(x / np.max(np.abs(x)))


def log_sweep(seconds: float, f0: float = 200.0, f1: float = 7000.0) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    k = math.log(f1 / f0)
    phase = 2 * math.pi * f0 * seconds / k * (np.exp(t / seconds * k) - 1)
    return fade(np.sin(phase))


def fade(x: np.ndarray, ms: float = 20.0) -> np.ndarray:
    n = int(RATE * ms / 1000)
    if len(x) > 2 * n:
        ramp = np.linspace(0, 1, n)
        x = x.copy()
        x[:n] *= ramp
        x[-n:] *= ramp[::-1]
    return x


def normalize(x: np.ndarray, target_rms: float = TARGET_RMS) -> np.ndarray:
    r = math.sqrt(float(np.mean(x * x))) or 1.0
    y = x * (target_rms / r)
    peak = float(np.max(np.abs(y))) or 1.0
    if peak > PEAK_LIMIT:
        y *= PEAK_LIMIT / peak
    return y


def load_any(path: str) -> np.ndarray:
    """Any format the Mac can open, resampled to 16 kHz mono via afconvert."""
    if path.lower().endswith(".wav"):
        try:
            pcm, rate, ch = read_wav(path)
            x = np.frombuffer(pcm, dtype=np.int16).astype(np.float64) / 32768.0
            if ch > 1:
                x = x.reshape(-1, ch).mean(axis=1)
            if rate == RATE:
                return x
        except ValueError:
            pass
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        out = tmp.name
    subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{RATE}", "-c", "1", path, out],
                   check=True, capture_output=True)
    pcm, _, _ = read_wav(out)
    os.unlink(out)
    return np.frombuffer(pcm, dtype=np.int16).astype(np.float64) / 32768.0


def trim_silence(x: np.ndarray, thresh_db: float = -40.0) -> np.ndarray:
    frame = 160
    n = (len(x) // frame) * frame
    if n == 0:
        return x
    fr = np.sqrt(np.mean(x[:n].reshape(-1, frame) ** 2, axis=1))
    loud = np.where(20 * np.log10(fr + 1e-9) > thresh_db)[0]
    if loud.size == 0:
        return x
    a = max(0, (loud[0] - 2) * frame)
    b = min(len(x), (loud[-1] + 3) * frame)
    return x[a:b]


def tts(text: str, voice: str = "Samantha") -> np.ndarray:
    with tempfile.NamedTemporaryFile(suffix=".aiff", delete=False) as tmp:
        out = tmp.name
    subprocess.run(["say", "-v", voice, "-o", out, text], check=True)
    x = load_any(out)
    os.unlink(out)
    return x


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="standard")
    ap.add_argument("--speech-dir", help="folder of recordings to use as the speech part")
    ap.add_argument("--speech-count", type=int, default=5)
    ap.add_argument("--interferer-dir", help="second talker's recordings -> stereo two-speaker clip")
    ap.add_argument("--no-sweep", action="store_true")
    ap.add_argument("--out-dir", default=os.path.join(PROJECT, "clips"))
    args = ap.parse_args()

    speech: list[tuple[str, np.ndarray]] = []
    if args.speech_dir:
        files = sorted(f for ext in ("wav", "m4a", "aiff", "mp3") for f in glob.glob(os.path.join(args.speech_dir, f"*.{ext}")))
        if not files:
            sys.exit(f"no audio files in {args.speech_dir}")
        for f in files[: args.speech_count]:
            speech.append((os.path.basename(f), trim_silence(load_any(f))))
    else:
        for i, phrase in enumerate(["Hey Vision", "Hey Vision, what's in front of me",
                                    "Hey Vision", "Read this sign", "Hey Vision"][: args.speech_count]):
            speech.append((f"tts_{i + 1}: {phrase}", trim_silence(tts(phrase))))

    interf: list[tuple[str, np.ndarray]] = []
    if args.interferer_dir:
        files = sorted(f for ext in ("wav", "m4a", "aiff", "mp3") for f in glob.glob(os.path.join(args.interferer_dir, f"*.{ext}")))
        if not files:
            sys.exit(f"no audio files in {args.interferer_dir}")
        for f in files[: args.speech_count]:
            interf.append((os.path.basename(f), trim_silence(load_any(f))))
    stereo = bool(interf)

    # (name, kind, left, right, text); mono clips use left only
    def mono(name, kind, x, text=""):
        return (name, kind, x, None, text)

    parts = [
        mono("silence_1", "silence", silence(1.0)),
        ("noise", "noise", normalize(pink_noise(3.0)), normalize(pink_noise(3.0, seed=11)) if stereo else None, "pink noise"),
        mono("silence_2", "silence", silence(1.0)),
    ]
    for i, (label, x) in enumerate(speech):
        parts.append(mono(f"speech_{i + 1}", "speech", normalize(x), label))
        parts.append(mono(f"gap_{i + 1}", "silence", silence(0.7)))
    if stereo:
        parts.append(mono("silence_i", "silence", silence(0.5)))
        for i, (label, x) in enumerate(interf):
            parts.append((f"interf_{i + 1}", "interf", None, normalize(x), label))
            parts.append(mono(f"igap_{i + 1}", "silence", silence(0.7)))
        parts.append(mono("silence_b", "silence", silence(0.5)))
        # both at once: wearer clip over a continuous interferer bed built from the interferer clips
        bed = np.concatenate([normalize(x) for _, x in interf])
        for i, (label, x) in enumerate(speech):
            w = normalize(x)
            pos = (i * 7919) % max(1, len(bed) - len(w))
            parts.append((f"both_{i + 1}", "both", w, bed[pos: pos + len(w)] if len(bed) >= len(w) else np.resize(bed, len(w)), label))
            parts.append(mono(f"bgap_{i + 1}", "silence", silence(0.7)))
    parts.append(mono("silence_3", "silence", silence(0.3)))
    if not args.no_sweep:
        parts.append(mono("sweep", "sweep", normalize(log_sweep(4.0)), "log sweep 200 Hz - 7 kHz"))
        parts.append(mono("silence_4", "silence", silence(1.0)))

    os.makedirs(args.out_dir, exist_ok=True)
    wav_path = os.path.join(args.out_dir, f"{args.name}.wav")
    cue_path = os.path.join(args.out_dir, f"{args.name}_cues.csv")

    left_parts: list[np.ndarray] = []
    right_parts: list[np.ndarray] = []
    t = 0.0
    with open(cue_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "kind", "start_s", "end_s", "text"])
        for name, kind, xl, xr, text in parts:
            n = len(xl) if xl is not None else len(xr)
            dur = n / RATE
            w.writerow([name, kind, f"{t:.3f}", f"{t + dur:.3f}", text])
            left_parts.append(xl if xl is not None else np.zeros(n))
            right_parts.append((xr if xr is not None else np.zeros(n))[:n] if stereo else np.zeros(0))
            t += dur
    yl = np.concatenate(left_parts)
    if stereo:
        yr = np.concatenate([np.resize(r, len(l)) if len(r) else np.zeros(len(l)) for l, r in zip(left_parts, right_parts)])
        inter = np.empty(2 * len(yl))
        inter[0::2] = yl
        inter[1::2] = yr
        pcm = np.clip(inter * 32767, -32768, 32767).astype("<i2").tobytes()
        write_wav(wav_path, pcm, RATE, 2)
    else:
        pcm = np.clip(yl * 32767, -32768, 32767).astype("<i2").tobytes()
        write_wav(wav_path, pcm, RATE, 1)
    print(f"wrote {wav_path}  ({t:.1f} s, {len(speech)} speech clips{', ' + str(len(interf)) + ' interferer clips, STEREO' if stereo else ''})")
    print(f"wrote {cue_path}")


if __name__ == "__main__":
    main()
