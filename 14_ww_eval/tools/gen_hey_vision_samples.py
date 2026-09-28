#!/usr/bin/env python3
"""Generate synthetic "hey vision" (positive) or non-wake-word (negative) test clips
with macOS `say`.

Each clip is 16 kHz, 16-bit, mono WAV (the model's input format), varied in
voice, speaking rate, pitch/speed, level, background noise and the silence
around the phrase. A manifest.csv records the parameters of every clip.

    python3 tools/gen_hey_vision_samples.py [--count 50] [--out test_samples/hey_vision]
    python3 tools/gen_hey_vision_samples.py --kind negative   # -> test_samples/not_hey_vision
"""
import argparse
import csv
import random
import subprocess
import tempfile
import wave
from pathlib import Path

import numpy as np

SR = 16000
PHRASE = "hey vision"

# Negatives: every phrase is made of ordinary English words.
# 25 near-misses (similar sounds / other assistant-style addresses), then 25 unrelated phrases.
NEGATIVES = [
    "hey mission", "hey division", "hey decision", "hey television", "hey revision",
    "hey provision", "hey precision", "hey fusion", "hey wisdom", "hey visa",
    "hey envision", "hey listen", "hey computer", "hey vivid", "hey visible",
    "hey visit", "hey vista", "hey assistant", "hey camera", "hey system",
    "hey winter", "hey vinyl", "okay vision", "hi vision", "vision",
    "turn on the lights", "what time is it", "good morning everyone",
    "the weather is nice today", "please open the door", "can you hear me",
    "I will see you tomorrow", "play some music", "testing one two three",
    "where did you put my keys", "the quick brown fox jumps over the lazy dog",
    "call me back later", "how are you doing", "set a timer for five minutes",
    "thank you very much", "let us grab lunch", "I need a coffee",
    "the meeting starts at noon", "close the window please", "what a beautiful day",
    "remind me to buy milk", "one two three four five", "see you later",
    "that sounds great", "are we still on for tonight",
]

# Clear English voices only (legacy robotic voices like Fred/Ralph/Albert/Kathy dropped).
VOICES = [
    "Samantha", "Daniel", "Karen", "Moira", "Rishi", "Tessa", "Tara", "Aman",
    "Eddy (English (US))", "Flo (English (US))", "Reed (English (UK))",
    "Sandy (English (UK))", "Shelley (English (US))", "Rocko (English (UK))",
    "Grandma (English (US))", "Grandpa (English (US))",
    "Eddy (English (UK))", "Flo (English (UK))", "Reed (English (US))",
    "Sandy (English (US))", "Shelley (English (UK))", "Rocko (English (US))",
    "Grandma (English (UK))", "Grandpa (English (UK))",
]


def synth(voice: str, rate: int, text: str, path: Path) -> np.ndarray:
    subprocess.run(
        ["say", "-v", voice, "-r", str(rate), "-o", str(path),
         "--file-format=WAVE", "--data-format=LEI16@16000", text],
        check=True,
    )
    with wave.open(str(path)) as w:
        assert w.getframerate() == SR and w.getnchannels() == 1
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return pcm.astype(np.float32) / 32768.0


def trim(x: np.ndarray, rel: float = 0.02, pad_s: float = 0.08) -> np.ndarray:
    idx = np.flatnonzero(np.abs(x) > rel * np.max(np.abs(x)))
    if not idx.size:
        return x
    pad = int(pad_s * SR)
    return x[max(idx[0] - pad, 0):idx[-1] + 1 + pad]


def resample(x: np.ndarray, factor: float) -> np.ndarray:
    """Linear-interp resample; factor>1 speeds up and raises pitch."""
    n = int(len(x) / factor)
    return np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x).astype(np.float32)


def pink_noise(n: int, rng: np.random.Generator) -> np.ndarray:
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(len(spec), dtype=np.float32)
    f[0] = 1
    y = np.fft.irfft(spec / np.sqrt(f), n)
    return (y / np.std(y)).astype(np.float32)


def write_wav(path: Path, x: np.ndarray) -> None:
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--kind", choices=["positive", "negative"], default="positive")
    ap.add_argument("--out", default=None)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    neg = args.kind == "negative"
    prefix = "not_hey_vision" if neg else "hey_vision"
    out = Path(args.out or f"test_samples/{prefix}")
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    nrng = np.random.default_rng(args.seed)
    rows = []

    with tempfile.TemporaryDirectory() as tmp:
        for i in range(args.count):
            voice = VOICES[i % len(VOICES)]
            rate = rng.randint(140, 190)          # words per minute (kept moderate so words stay intelligible)
            speed = rng.uniform(0.96, 1.05)        # extra resample: pitch+tempo shift
            gain_db = rng.uniform(-15, -3)
            snr_db = rng.choice([None, 30, 25, 20, 15])
            pre = rng.uniform(0.6, 2.0)
            post = rng.uniform(0.6, 2.0)

            phrase = NEGATIVES[i % len(NEGATIVES)] if neg else PHRASE
            x = trim(synth(voice, rate, phrase, Path(tmp) / "t.wav"))
            x = resample(x, speed)
            x /= max(np.max(np.abs(x)), 1e-6)
            x *= 10 ** (gain_db / 20)

            clip = np.concatenate([
                np.zeros(int(pre * SR), np.float32), x,
                np.zeros(int(post * SR), np.float32),
            ])
            if snr_db is not None:
                noise = pink_noise(len(clip), nrng)
                sig_rms = np.sqrt(np.mean(x ** 2))
                clip = clip + noise * sig_rms / (10 ** (snr_db / 20))
            else:
                clip = clip + nrng.standard_normal(len(clip)).astype(np.float32) * 1e-4

            name = f"{prefix}_{i + 1:02d}.wav"
            write_wav(out / name, clip)
            rows.append({
                "file": name, "phrase": phrase, "voice": voice, "rate_wpm": rate,
                "speed": f"{speed:.3f}", "gain_db": f"{gain_db:.1f}",
                "snr_db": "" if snr_db is None else snr_db,
                "onset_s": f"{pre:.2f}", "speech_s": f"{len(x) / SR:.2f}",
                "duration_s": f"{len(clip) / SR:.2f}",
            })
            print(f"{name}  {voice:<22} {rate}wpm  snr={snr_db}  '{phrase}'")

    with open(out / "manifest.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"\nWrote {len(rows)} clips + manifest.csv to {out}")


if __name__ == "__main__":
    main()
