"""
Offline DSP for the evaluation: beamformer variants built from the three raw
microphone recordings, plus the measurements the reports are made of.

Everything here is numpy only (no scipy), so it runs on the stock macOS
Python once numpy and matplotlib are installed.

Paths
-----
raw_left / raw_center / raw_right   the microphones as captured
lr_mix          (L + R) / 2                       the 2-mic prototype of 13_finalProto
das             delay-and-sum of all three, same integer arithmetic as the firmware
das_<name>      the same with another delay set (see DAS_VARIANTS)
das_auto        delays fitted from the wearer's speech (mirrors beamform_cal.c)
gsc_auto        das_auto + canceller with the fitted R gain trim: the shipping configuration
diff_lr         (L - R) / 2                       first-order dipole along the frame:
                                                  a NULL at broadside, so this is the
                                                  "everything except the wearer" reference
gsc             das minus the part of it predictable from diff_lr (offline Wiener fit):
                an upper bound on what an adaptive null-steering stage could add
gsc_nlms        the deployable version: causal, gated NLMS, mirrors beamform_gsc.c

Measurements
------------
align_clip      where the played clip sits in the recording (cross-correlation)
segment_levels  RMS per clip segment (noise burst, speech, sweep, silence) per path
band_levels     third-octave band levels over a segment (frequency response / beam
                pattern vs frequency)
tdoa            inter-mic delay from GCC-PHAT, turned into an angle of arrival
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

RATE = 16000
SPEED_OF_SOUND = 343.0

# Named delay sets (left, center, right) tried offline on every recording.
DAS_VARIANTS: Dict[str, Tuple[int, int, int]] = {
    "das": (0, 0, 0),           # broadside, what the device runs by default
    "das_c1": (0, 1, 0),        # center mic held back 1 sample: near-field mouth below the bridge
    "das_c2": (0, 2, 0),
}

THIRD_OCTAVE_CENTERS = [250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300]


# ---------------------------------------------------------------- helpers

def to_float(x: np.ndarray) -> np.ndarray:
    return x.astype(np.float64) / 32768.0


def dbfs(rms: float) -> float:
    return 20.0 * math.log10(max(rms, 1e-9))


def rms(x: np.ndarray) -> float:
    if x.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(x.astype(np.float64)))))


def frame_rms(x: np.ndarray, frame: int = 256) -> np.ndarray:
    n = (len(x) // frame) * frame
    if n == 0:
        return np.zeros(0)
    f = x[:n].astype(np.float64).reshape(-1, frame)
    return np.sqrt(np.mean(f * f, axis=1))


# ---------------------------------------------------------------- firmware-exact DAS

def delay_and_sum_int(mics: Sequence[np.ndarray], delays: Sequence[int],
                      gains_q15: Sequence[int] = (32767, 32767, 32767),
                      out_gain_q15: int = 32767) -> np.ndarray:
    """Bit-exact replica of src/beamformer_delay_sum.c on int16 arrays."""
    n = min(len(m) for m in mics)
    acc = np.zeros(n, dtype=np.int64)
    for m, d, g in zip(mics, delays, gains_q15):
        x = m[:n].astype(np.int64)
        if d > 0:
            x = np.concatenate([np.zeros(d, dtype=np.int64), x[:-d]])
        acc += (x * int(g)) >> 15          # arithmetic shift, like the C code
    # C integer division truncates toward zero; numpy // floors.
    avg = np.sign(acc) * (np.abs(acc) // len(mics))
    y = (avg * int(out_gain_q15)) >> 15
    return np.clip(y, -32768, 32767).astype(np.int16)


def lr_mix_int(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    n = min(len(left), len(right))
    s = left[:n].astype(np.int32) + right[:n].astype(np.int32)
    return (np.sign(s) * (np.abs(s) // 2)).astype(np.int16)


def dipole_int(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    n = min(len(left), len(right))
    s = left[:n].astype(np.int32) - right[:n].astype(np.int32)
    return (np.sign(s) * (np.abs(s) // 2)).astype(np.int16)


def wiener_cancel(primary: np.ndarray, reference: np.ndarray, taps: int = 16) -> np.ndarray:
    """primary - FIR(reference), FIR fitted by least squares over the whole
    recording. Offline and non-causal in the fit, so it is an upper bound on
    an adaptive canceller, not a deployable filter."""
    n = min(len(primary), len(reference))
    p = to_float(primary[:n])
    r = to_float(reference[:n])
    if n < taps * 4 or rms(r) < 1e-6:
        return primary[:n].copy()
    # Build the Toeplitz data matrix lazily via FFT-free sliding windows.
    half = taps // 2
    rp = np.concatenate([np.zeros(half), r, np.zeros(taps)])
    X = np.lib.stride_tricks.sliding_window_view(rp, taps)[:n]
    w, *_ = np.linalg.lstsq(X, p, rcond=None)
    y = p - X @ w
    return np.clip(y * 32768.0, -32768, 32767).astype(np.int16)


def gsc_nlms(primary: np.ndarray, left: np.ndarray, right: np.ndarray, taps: int = 16,
             mu: float = 0.05, gate_ratio_db: float = -6.0, gain_l: float = 1.0, gain_r: float = 1.0,
             w_max: float = 2.0, frame: int = 256) -> np.ndarray:
    """Causal, sample-by-sample NLMS sidelobe canceller: the same arithmetic as
    beamform_gsc.c in 15_3micBF (float), so what the rig scores is what the
    device would run.

      r[n] = (gL*L[n] - gR*R[n]) / 2        target-free reference (mouth is symmetric)
      e[n] = y[n] - w . r[n-15..n]           output
      w   += mu * e[n] * r_vec / (|r_vec|^2 + eps)   only while the gate is open

    The gate opens for a frame when the reference power is within
    `gate_ratio_db` of the primary power (an interferer is present); during the
    wearer's own speech the reference is small and the weights freeze, so a
    slight misalignment cannot turn into speech cancellation. |w| is capped.
    """
    n = min(len(primary), len(left), len(right))
    y = to_float(primary[:n])
    r = (gain_l * to_float(left[:n]) - gain_r * to_float(right[:n])) * 0.5
    w = np.zeros(taps)
    hist = np.zeros(taps)
    out = np.zeros(n)
    eps = 1e-6
    gate_ratio = 10 ** (gate_ratio_db / 10)
    for f0 in range(0, n, frame):
        f1 = min(n, f0 + frame)
        py = float(np.mean(y[f0:f1] ** 2)) + eps
        pr = float(np.mean(r[f0:f1] ** 2)) + eps
        adapt = pr > gate_ratio * py
        for i in range(f0, f1):
            hist = np.roll(hist, 1)
            hist[0] = r[i]
            e = y[i] - float(np.dot(w, hist))
            out[i] = e
            if adapt:
                w += mu * e * hist / (float(np.dot(hist, hist)) + eps)
        norm = float(np.linalg.norm(w))
        if norm > w_max:
            w *= w_max / norm
    return np.clip(out * 32768.0, -32768, 32767).astype(np.int16)


def estimate_alignment(left: np.ndarray, center: np.ndarray, right: np.ndarray,
                       speech_segs: Optional[List[Tuple[int, int]]] = None, max_lag: int = 2,
                       center_prior: int = 1, margin: float = 0.05) -> Dict[str, float]:
    """Mirror of 15_3micBF/src/beamform_cal.c: integer L/R lag, centre lag and
    R gain trim from the wearer's own speech (the frames given, or the whole
    recording). Returns delays in the firmware's convention (samples to hold
    each mic back) plus the raw numbers."""
    segs = speech_segs or [(0, min(len(left), len(center), len(right)))]
    L = np.concatenate([left[a:b].astype(np.int64) for a, b in segs])
    C = np.concatenate([center[a:b].astype(np.int64) for a, b in segs])
    R = np.concatenate([right[a:b].astype(np.int64) for a, b in segs])
    n = len(L)
    if n < 4 * max_lag + 16:
        return {"valid": False}
    sl = slice(max_lag, n - max_lag)
    r_lr = {k: int(np.dot(L[sl], R[max_lag + k: n - max_lag + k])) for k in range(-max_lag, max_lag + 1)}
    c_lag = {k: int(np.dot(C[max_lag - k: n - max_lag - k], L[sl] + R[sl])) for k in range(0, max_lag + 1)}
    e_l = float(np.dot(L[sl], L[sl]))
    e_r = float(np.dot(R[sl], R[sl]))
    # The prior (lag 0 for L/R, `center_prior` for C) only loses to a lag that
    # beats it by `margin`, as in the firmware.
    best = max(r_lr, key=r_lr.get)
    if best != 0 and not r_lr[best] > r_lr[0] * (1 + margin):
        best = 0
    coh = r_lr[best] / math.sqrt(e_l * e_r) if e_l > 0 and e_r > 0 else 0.0
    bc = max(c_lag, key=c_lag.get)
    if bc != center_prior and not c_lag[bc] > c_lag[center_prior] * (1 + margin):
        bc = center_prior
    g = math.sqrt(e_l / e_r) if e_r > 0 else 1.0
    g = min(1.413, max(0.708, g))
    return {"valid": coh >= 0.5, "lr_lag": best, "coherence": round(coh, 3),
            "delay_left": max(best, 0), "delay_right": max(-best, 0), "delay_center": bc,
            "gain_right": round(g, 4)}


def build_paths(left: np.ndarray, center: np.ndarray, right: np.ndarray,
                device_bf: Optional[np.ndarray] = None,
                speech_segs: Optional[List[Tuple[int, int]]] = None) -> Dict[str, np.ndarray]:
    n = min(len(left), len(center), len(right))
    L, C, R = left[:n], center[:n], right[:n]
    paths: Dict[str, np.ndarray] = {
        "raw_left": L, "raw_center": C, "raw_right": R,
        "lr_mix": lr_mix_int(L, R),
    }
    for name, d in DAS_VARIANTS.items():
        paths[name] = delay_and_sum_int((L, C, R), d)
    # Per-wearer fit, as the firmware does it after a wake word: delays from the
    # speech itself, R gain trim into the canceller reference.
    fit = estimate_alignment(L, C, R, speech_segs)
    if fit.get("valid"):
        d = (fit["delay_left"], fit["delay_center"], fit["delay_right"])
        paths["das_auto"] = delay_and_sum_int((L, C, R), d)
        paths["gsc_auto"] = gsc_nlms(paths["das_auto"], L, R, gain_r=fit["gain_right"])
    paths["diff_lr"] = dipole_int(L, R)
    paths["gsc"] = wiener_cancel(paths["das"], paths["diff_lr"])          # offline upper bound
    paths["gsc_nlms"] = gsc_nlms(paths["das"], L, R)                        # what the device can run
    if device_bf is not None and len(device_bf) >= n:
        paths["bf_device"] = device_bf[:n]
    return paths


# ---------------------------------------------------------------- alignment

def xcorr_lag(a: np.ndarray, b: np.ndarray, max_lag: Optional[int] = None) -> Tuple[int, float]:
    """Lag that maximises the normalized cross-correlation of b against a
    (positive lag: b occurs later in a). FFT based."""
    n = len(a) + len(b) - 1
    nfft = 1 << (n - 1).bit_length()
    A = np.fft.rfft(a, nfft)
    B = np.fft.rfft(b, nfft)
    cc = np.fft.irfft(A * np.conj(B), nfft)
    cc = np.concatenate([cc[-(len(b) - 1):], cc[:len(a)]])  # lags -(len(b)-1) .. len(a)-1
    lags = np.arange(-(len(b) - 1), len(a))
    if max_lag is not None:
        keep = np.abs(lags) <= max_lag
        cc, lags = cc[keep], lags[keep]
    i = int(np.argmax(cc))
    norm = (np.linalg.norm(a) * np.linalg.norm(b)) or 1.0
    return int(lags[i]), float(cc[i] / norm)


def align_clip(recording: np.ndarray, clip: np.ndarray) -> Tuple[int, float]:
    """Sample offset of the clip's start inside the recording, and a confidence
    0..1. Uses the envelopes so level and coloration differences do not matter."""
    frame = 64
    er = frame_rms(recording, frame)
    ec = frame_rms(clip, frame)
    er = er - er.mean()
    ec = ec - ec.mean()
    lag, conf = xcorr_lag(er, ec)
    coarse = lag * frame
    # refine on the waveform within +-frame
    lo = max(0, coarse - 2 * frame)
    seg = to_float(recording[lo: lo + len(clip) + 4 * frame])
    if len(seg) > frame * 8:
        fine, _ = xcorr_lag(seg, to_float(clip), max_lag=None)
        if abs(fine - (coarse - lo)) <= 2 * frame:
            return lo + fine, conf
    return coarse, conf


# ---------------------------------------------------------------- measurements

@dataclass
class Segment:
    name: str
    kind: str           # silence | noise | speech | sweep | tone
    start_s: float
    end_s: float
    text: str = ""

    def slice(self, offset: int, n_total: int) -> Tuple[int, int]:
        a = offset + int(round(self.start_s * RATE))
        b = offset + int(round(self.end_s * RATE))
        return max(0, a), max(0, min(n_total, b))


def segment_levels(paths: Dict[str, np.ndarray], segments: List[Segment], offset: int) -> Dict[str, Dict[str, float]]:
    """{path: {segment_name: dBFS}}"""
    out: Dict[str, Dict[str, float]] = {}
    for name, x in paths.items():
        row: Dict[str, float] = {}
        for seg in segments:
            a, b = seg.slice(offset, len(x))
            if b - a > RATE // 20:
                # trim 50 ms either side so speaker ramps and alignment error do not count
                t = RATE // 20
                row[seg.name] = dbfs(rms(to_float(x[a + t: b - t])))
        out[name] = row
    return out


def speech_snr(x: np.ndarray, speech_segs: List[Tuple[int, int]], silence_segs: List[Tuple[int, int]]) -> Optional[float]:
    """Energy-weighted speech level vs silence floor, in dB. Within each speech
    segment only the loudest 60 % of 16 ms frames count as speech so pauses
    between words do not drag the level down."""
    sp: List[np.ndarray] = []
    for a, b in speech_segs:
        fr = frame_rms(to_float(x[a:b]))
        if fr.size:
            thr = np.percentile(fr, 40)
            sp.append(fr[fr >= thr])
    si = [frame_rms(to_float(x[a:b])) for a, b in silence_segs]
    si = [s for s in si if s.size]
    if not sp or not si:
        return None
    s_level = math.sqrt(float(np.mean(np.square(np.concatenate(sp)))))
    n_level = math.sqrt(float(np.mean(np.square(np.concatenate(si)))))
    return dbfs(s_level) - dbfs(n_level)


def band_levels(x: np.ndarray, centers: Sequence[int] = THIRD_OCTAVE_CENTERS, nfft: int = 1024) -> Dict[int, float]:
    """Average third-octave band level (dBFS) over x, via a Hann STFT."""
    xf = to_float(x)
    if len(xf) < nfft:
        return {}
    hop = nfft // 2
    win = np.hanning(nfft)
    n = (len(xf) - nfft) // hop + 1
    frames = np.lib.stride_tricks.as_strided(xf, shape=(n, nfft), strides=(xf.strides[0] * hop, xf.strides[0]))
    spec = np.abs(np.fft.rfft(frames * win, axis=1)) ** 2
    psd = spec.mean(axis=0) / (np.sum(win ** 2) * RATE)
    freqs = np.fft.rfftfreq(nfft, 1.0 / RATE)
    out: Dict[int, float] = {}
    for fc in centers:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        m = (freqs >= lo) & (freqs < hi)
        if m.any():
            p = float(np.sum(psd[m]) * (freqs[1] - freqs[0]))
            out[fc] = 10.0 * math.log10(max(p, 1e-18))
    return out


def spectrogram(x: np.ndarray, nfft: int = 512, hop: int = 128) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    xf = to_float(x)
    if len(xf) < nfft:
        return np.zeros((nfft // 2 + 1, 1)), np.fft.rfftfreq(nfft, 1 / RATE), np.zeros(1)
    win = np.hanning(nfft)
    n = (len(xf) - nfft) // hop + 1
    frames = np.lib.stride_tricks.as_strided(xf, shape=(n, nfft), strides=(xf.strides[0] * hop, xf.strides[0]))
    S = np.abs(np.fft.rfft(frames * win, axis=1)).T
    S_db = 20 * np.log10(S + 1e-7)
    return S_db, np.fft.rfftfreq(nfft, 1 / RATE), np.arange(n) * hop / RATE


def gcc_phat(a: np.ndarray, b: np.ndarray, max_lag: int = 12, interp: int = 8) -> Tuple[float, float]:
    """Samples by which b LEADS a (positive: b hears the sound first), fractional,
    and the sharpness of the correlation peak (bigger is more trustworthy)."""
    af, bf = to_float(a), to_float(b)
    n = len(af) + len(bf)
    nfft = 1 << (n - 1).bit_length()
    A = np.fft.rfft(af, nfft)
    B = np.fft.rfft(bf, nfft)
    R = A * np.conj(B)
    R /= np.abs(R) + 1e-12
    cc = np.fft.irfft(R, nfft * interp)
    max_shift = max_lag * interp
    cc = np.concatenate([cc[-max_shift:], cc[: max_shift + 1]])
    i = int(np.argmax(np.abs(cc)))
    shift = (i - max_shift) / interp
    peak = float(np.abs(cc[i]) / (np.mean(np.abs(cc)) + 1e-12))
    return shift, peak


def tdoa_angle(delay_samples: float, spacing_m: float) -> Optional[float]:
    """Angle of arrival (deg, 0 = broadside, positive toward the mic that
    hears the sound first = right) for a pair `spacing_m` apart."""
    s = delay_samples * SPEED_OF_SOUND / RATE / spacing_m
    if abs(s) > 1.0:
        return None
    return math.degrees(math.asin(s))


# ---------------------------------------------------------------- scoring

def directivity_index(levels_by_angle: Dict[float, float]) -> Optional[float]:
    """Crude DI from a 2-D polar: on-axis power over the mean power across
    the measured angles (assumes the angles sample the half plane evenly)."""
    if 0.0 not in levels_by_angle or len(levels_by_angle) < 3:
        return None
    on = 10 ** (levels_by_angle[0.0] / 10)
    mean = np.mean([10 ** (v / 10) for v in levels_by_angle.values()])
    return 10 * math.log10(on / mean) if mean > 0 else None
