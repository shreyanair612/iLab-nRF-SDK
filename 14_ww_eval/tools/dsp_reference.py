"""
Bit-exact Python mirror of the firmware's path arithmetic (src/eval_paths.c and
src/beamformer_delay_sum.c).

Why this exists: the device streams the raw microphones during a run and
reports a CRC32 of every processed path it computed. The host rebuilds each
path from the raw channels with these functions and compares CRCs, so the
processed WAV files it writes are provably the same samples the wake-word
model saw on the device. If a CRC does not match, the run is flagged rather
than trusted.

Pure Python integers are used throughout so truncation, shifts, and
saturation behave exactly like the C code (C division truncates toward zero;
Python's // floors, hence tdiv()).
"""
from __future__ import annotations

import array
import zlib
from typing import List, Sequence

INT16_MAX = 32767
INT16_MIN = -32768
MAX_DELAY = 64


def tdiv(a: int, b: int) -> int:
    """C-style integer division (truncate toward zero) for b > 0."""
    q = abs(a) // b
    return q if a >= 0 else -q


def sat16(x: int) -> int:
    return INT16_MAX if x > INT16_MAX else INT16_MIN if x < INT16_MIN else x


def bytes_to_samples(pcm: bytes) -> array.array:
    a = array.array("h")
    a.frombytes(pcm[: len(pcm) - (len(pcm) % 2)])
    return a


def samples_to_bytes(samples: Sequence[int]) -> bytes:
    a = array.array("h", samples)
    return a.tobytes()


def crc32(pcm: bytes) -> int:
    """IEEE CRC32 over the little-endian int16 bytes; matches am_crc32_update."""
    return zlib.crc32(pcm) & 0xFFFFFFFF


def lr_mix(left: Sequence[int], right: Sequence[int]) -> List[int]:
    """RAW_LR_MIX: out = left/2 + right/2 with C truncation. Never overflows."""
    return [tdiv(l, 2) + tdiv(r, 2) for l, r in zip(left, right)]


def reduce_3ch(left, right, center, mode: str) -> List[int]:
    if mode == "center_only":
        return list(center)
    if mode == "lr_average":
        return lr_mix(left, right)
    if mode == "lc_average":
        return lr_mix(left, center)
    if mode == "rc_average":
        return lr_mix(right, center)
    if mode == "lrc_average":
        return [tdiv(l, 3) + tdiv(r, 3) + tdiv(c, 3) for l, r, c in zip(left, right, center)]
    raise ValueError(f"unknown reduce mode {mode}")


def delay_and_sum(channels: Sequence[Sequence[int]], delays: Sequence[int],
                  gains_q15: Sequence[int] | None = None, frame: int = 256) -> tuple[List[int], int]:
    """Baseline integer delay-and-sum, identical to bf_process():

        out[n] = sat16( (sum_c (x_c[n - delay_c] * gain_c) >> 15) / N )

    Processing is done frame by frame with a per-channel history ring of
    MAX_DELAY samples, exactly like the firmware, so the first delay_c samples
    of a delayed channel read as zero. Returns (samples, clip_count).
    """
    nch = len(channels)
    if gains_q15 is None:
        gains_q15 = [INT16_MAX] * nch
    for d in delays:
        if d < 0 or d > MAX_DELAY:
            raise ValueError("delay out of range")
    n = min(len(c) for c in channels)
    out: List[int] = []
    clips = 0
    history = [[0] * MAX_DELAY for _ in range(nch)]
    head = [0] * nch

    for start in range(0, n, frame):
        end = min(start + frame, n)
        fl = end - start
        for i in range(fl):
            acc = 0
            for c in range(nch):
                d = delays[c]
                if d == 0 or i >= d:
                    s = channels[c][start + i - d]
                else:
                    back = d - i
                    s = history[c][(head[c] + MAX_DELAY - back) % MAX_DELAY]
                acc += (s * gains_q15[c]) >> 15
            acc = tdiv(acc, nch)
            y = sat16(acc)
            if y == INT16_MAX or y == INT16_MIN:
                clips += 1
            out.append(y)
        for c in range(nch):
            keep_from = fl - MAX_DELAY if fl > MAX_DELAY else 0
            for i in range(keep_from, fl):
                history[c][head[c]] = channels[c][start + i]
                head[c] = (head[c] + 1) % MAX_DELAY
    return out, clips


def build_path(path: str, left_pcm: bytes, right_pcm: bytes, center_pcm: bytes | None,
               cfg: dict) -> tuple[bytes, dict]:
    """Rebuild a processed path from raw channel bytes using the run's config.

    cfg is the RUN_CONFIG JSON from the device. Returns (pcm_bytes, info)
    where info holds the parameters used and the CRC32.
    """
    left = bytes_to_samples(left_pcm)
    right = bytes_to_samples(right_pcm)
    center = bytes_to_samples(center_pcm) if center_pcm else None
    n = min(len(left), len(right))
    left, right = left[:n], right[:n]
    if center is not None:
        n = min(n, len(center))
        left, right, center = left[:n], right[:n], center[:n]
    pcfg = {p["name"]: p for p in cfg.get("paths", [])}
    info = {"path": path}

    if path == "raw_left":
        out = list(left)
    elif path == "raw_right":
        out = list(right)
    elif path == "raw_lr_mix":
        out = lr_mix(left, right)
    elif path == "bf_lr":
        p = pcfg.get("bf_lr", {})
        delays = [p.get("dl", 0), p.get("dr", 0)]
        gain = p.get("gain", INT16_MAX)
        out, clips = delay_and_sum([left, right], delays, [gain, gain], cfg.get("frame", 256))
        info.update({"delays": delays, "gain_q15": gain, "clips": clips})
    elif path == "raw_3ch":
        if center is None:
            raise ValueError("raw_3ch needs a center channel")
        out = reduce_3ch(left, right, center, cfg.get("reduce", "lrc_average"))
        info["reduce"] = cfg.get("reduce", "lrc_average")
    elif path == "bf_3ch":
        if center is None:
            raise ValueError("bf_3ch needs a center channel")
        p = pcfg.get("bf_3ch", {})
        delays = [p.get("dl", 0), p.get("dr", 0), p.get("dc", 0)]
        gain = p.get("gain", INT16_MAX)
        out, clips = delay_and_sum([left, right, center], delays, [gain] * 3, cfg.get("frame", 256))
        info.update({"delays": delays, "gain_q15": gain, "clips": clips})
    else:
        raise ValueError(f"unknown path {path}")

    pcm = samples_to_bytes(out)
    info["crc32"] = f"{crc32(pcm):08x}"
    info["samples"] = len(out)
    return pcm, info
