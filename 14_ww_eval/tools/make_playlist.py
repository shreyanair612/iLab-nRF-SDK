#!/usr/bin/env python3
"""
Builds the audio you play from the target speaker during a test.

    python3 tools/make_playlist.py --tts
        50 "Hey Vision" clips and 50 decoy phrases, spoken by the Mac's
        built-in voices at different speeds.

    python3 tools/make_playlist.py --wake-dir my_recordings/wake --decoy-dir my_recordings/decoys
        Your own recordings instead (WAV, M4A, AIFF, MP3: anything the Mac can
        open). Recommended for the real study; see RIG_SETUP.md.

Writes to playlists/<name>/:

    wake_50.wav             the 50 wake words, one every few seconds
    decoys_50.wav           the 50 decoys, same spacing
    calibration_noise.wav   30 s of pink noise at the same loudness as the clips,
                            for setting the speaker volume with a sound meter
    cues_wake_50.csv        when each clip starts, and what it says
    cues_decoys_50.csv

Every clip is normalised to the same loudness, so the speaker volume is the only
thing that sets the level at the microphones.
"""
from __future__ import annotations

import argparse
import csv
import glob
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is missing. Install it with:  pip3 install numpy")

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 16000

WAKE_TEXTS = ["Hey Vision", "Hey, Vision", "hey vision"]

# Decoys: a mix of close sound-alikes (hard) and ordinary phrases (easy).
DECOY_TEXTS = [
    "Hey Vivian", "Hey Visual", "Hey Mission", "Hey Division", "Hey Decision",
    "Hey Revision", "Hey Television", "Hey Precision", "Hey Provision", "Hey Venison",
    "Hey Madison", "Hey Addison", "Hey Visa", "They envision", "A vision",
    "Hey Siri", "Hey there", "Hey Vicky", "What's the time", "Turn left here",
]

# Novelty and effect voices that would sound nothing like a person.
NOVELTY = {"Albert", "Bad News", "Bahh", "Bells", "Boing", "Bubbles", "Cellos", "Wobble",
           "Good News", "Jester", "Organ", "Superstar", "Trinoids", "Whisper", "Zarvox",
           "Fred", "Junior", "Ralph", "Kathy"}


def english_voices() -> list[str]:
    out = subprocess.run(["say", "-v", "?"], capture_output=True, text=True, check=True).stdout
    voices = []
    for line in out.splitlines():
        m = re.match(r"^(.+?)\s+([a-z]{2}_[A-Z]{2})\s+#", line)
        if not m:
            continue
        name, loc = m.group(1).strip(), m.group(2)
        base = name.split(" (")[0]
        if loc.startswith("en_") and base not in NOVELTY:
            voices.append(name)
    return voices


def read_mono16k(path: str) -> np.ndarray:
    """Any audio file the Mac can decode -> float32 mono at 16 kHz."""
    with tempfile.TemporaryDirectory() as d:
        tmp = os.path.join(d, "x.wav")
        subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{RATE}", "-c", "1", path, tmp],
                       check=True, capture_output=True)
        with wave.open(tmp) as w:
            data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32)
    return data / 32768.0


def tts_clip(text: str, voice: str, rate: int) -> np.ndarray:
    with tempfile.TemporaryDirectory() as d:
        tmp = os.path.join(d, "x.wav")
        subprocess.run(["say", "-v", voice, "-r", str(rate), "-o", tmp, "--file-format=WAVE",
                        f"--data-format=LEI16@{RATE}", text], check=True, capture_output=True)
        with wave.open(tmp) as w:
            data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32)
    return data / 32768.0


def trim(x: np.ndarray, thresh_db: float = -45.0) -> np.ndarray:
    """Drop leading and trailing near-silence so spacing is set by --gap alone."""
    if not len(x):
        return x
    env = np.convolve(np.abs(x), np.ones(160) / 160, mode="same")
    on = np.where(env > 10 ** (thresh_db / 20))[0]
    if not len(on):
        return x
    a, b = max(0, on[0] - 480), min(len(x), on[-1] + 800)
    return x[a:b]


def normalise(x: np.ndarray, target_dbfs: float) -> np.ndarray:
    """Scale to a speech-level RMS (measured over the voiced part), never clipping."""
    frame = 320
    n = len(x) // frame
    if n == 0:
        return x
    frames = x[: n * frame].reshape(n, frame)
    rms = np.sqrt((frames ** 2).mean(axis=1))
    voiced = rms[rms > rms.max() * 0.1]
    level = float(np.sqrt((voiced ** 2).mean())) if len(voiced) else float(rms.mean())
    if level <= 0:
        return x
    gain = 10 ** (target_dbfs / 20) / level
    peak = float(np.abs(x).max()) * gain
    if peak > 0.89:                     # keep peaks under -1 dBFS
        gain *= 0.89 / peak
    return x * gain


def pink_noise(seconds: float, target_dbfs: float, seed: int = 1) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = int(seconds * RATE)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / RATE)
    shape = np.zeros_like(f)
    band = (f >= 100) & (f <= 7000)
    shape[band] = 1 / np.sqrt(f[band])
    x = np.fft.irfft(spec * shape, n)
    x *= 10 ** (target_dbfs / 20) / np.sqrt((x ** 2).mean())
    fade = int(0.05 * RATE)
    x[:fade] *= np.linspace(0, 1, fade)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x


def write_wav(path: str, x: np.ndarray) -> None:
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def assemble(clips, lead: float, gap: float):
    """clips: list of (array, label, source). Returns audio and cue rows."""
    parts = [np.zeros(int(lead * RATE), dtype=np.float32)]
    cues, t = [], lead
    for i, (x, label, source) in enumerate(clips, 1):
        dur = len(x) / RATE
        cues.append({"index": i, "start_s": round(t, 3), "end_s": round(t + dur, 3),
                     "text": label, "source": source})
        parts += [x.astype(np.float32), np.zeros(int(gap * RATE), dtype=np.float32)]
        t += dur + gap
    return np.concatenate(parts), cues


def write_cues(path: str, cues) -> None:
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["index", "start_s", "end_s", "text", "source"])
        w.writeheader()
        w.writerows(cues)


def from_dir(folder: str, count: int, rng: random.Random):
    exts = ("*.wav", "*.m4a", "*.aif", "*.aiff", "*.mp3", "*.caf", "*.flac")
    files = sorted(sum((glob.glob(os.path.join(folder, e)) for e in exts), []))
    if not files:
        sys.exit(f"No audio files found in {folder}")
    if len(files) < count:
        print(f"note: {folder} has {len(files)} files; repeating them to reach {count}")
    picked = [files[i % len(files)] for i in range(count)]
    rng.shuffle(picked)
    return [(read_mono16k(p), os.path.splitext(os.path.basename(p))[0], os.path.basename(p)) for p in picked]


def from_tts(texts, count: int, voices, rng: random.Random):
    rates = [150, 175, 200, 225]
    combos = [(t, v, r) for v in voices for t in texts for r in rates]
    rng.shuffle(combos)
    # Spread across voices first so no single voice dominates.
    by_voice, out, seen = {}, [], set()
    for c in combos:
        by_voice.setdefault(c[1], []).append(c)
    while len(out) < count:
        progressed = False
        for v in voices:
            if by_voice.get(v) and len(out) < count:
                c = by_voice[v].pop()
                if (c[0], c[1]) in seen and any(by_voice.values()):
                    by_voice[v].insert(0, c)
                    continue
                seen.add((c[0], c[1]))
                out.append(c)
                progressed = True
        if not progressed:
            break
    clips = []
    for text, voice, rate in out:
        clips.append((tts_clip(text, voice, rate), text, f"{voice} @ {rate} wpm"))
    return clips


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--tts", action="store_true", help="use the Mac's built-in voices")
    src.add_argument("--wake-dir", help="folder of your own wake-word recordings")
    ap.add_argument("--decoy-dir", help="folder of your own decoy recordings (default: TTS decoys)")
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--gap", type=float, default=2.5, help="silence after each clip, seconds (keep above 1.5)")
    ap.add_argument("--lead", type=float, default=3.0, help="silence before the first clip, seconds")
    ap.add_argument("--level", type=float, default=-20.0, help="speech RMS level of every clip, dBFS")
    ap.add_argument("--name", default=None, help="output folder name under playlists/")
    ap.add_argument("--seed", type=int, default=2026)
    args = ap.parse_args()

    if args.gap < 1.5:
        sys.exit("--gap must be at least 1.5 s: the firmware ignores a second detection within 1.2 s.")
    rng = random.Random(args.seed)
    name = args.name or ("tts" if args.tts else "recorded")
    out = os.path.join(PROJECT, "playlists", name)
    os.makedirs(out, exist_ok=True)

    voices = english_voices()
    need_tts = args.tts or not args.decoy_dir
    if need_tts and not voices:
        sys.exit("No English voices found. Add some in System Settings > Accessibility > Spoken Content.")

    print("building wake words...")
    wake = from_tts(WAKE_TEXTS, args.count, voices, rng) if args.tts else from_dir(args.wake_dir, args.count, rng)
    print("building decoys...")
    decoys = from_dir(args.decoy_dir, args.count, rng) if args.decoy_dir else from_tts(DECOY_TEXTS, args.count, voices, rng)

    def prep(clips):
        return [(normalise(trim(x), args.level), label, s) for x, label, s in clips]

    for tag, clips in (("wake", prep(wake)), ("decoys", prep(decoys))):
        audio, cues = assemble(clips, args.lead, args.gap)
        write_wav(os.path.join(out, f"{tag}_{args.count}.wav"), audio)
        write_cues(os.path.join(out, f"cues_{tag}_{args.count}.csv"), cues)
        secs = len(audio) / RATE
        print(f"  {tag}_{args.count}.wav  {len(clips)} clips, {secs:.0f} s "
              f"({int(secs // 60)} min {int(secs % 60)} s)  ->  on the device: set_duration {math.ceil(secs) + 5}")

    write_wav(os.path.join(out, "calibration_noise.wav"), pink_noise(30, args.level))
    print(f"  calibration_noise.wav  30 s pink noise at the same loudness ({args.level:.0f} dBFS)")
    print(f"\nwritten to {os.path.relpath(out, PROJECT)}/")
    if args.tts:
        print(f"voices used: {len(voices)} ({', '.join(sorted({v.split(' (')[0] for v in voices}))})")


if __name__ == "__main__":
    main()
