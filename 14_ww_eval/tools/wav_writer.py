"""
WAV output for received PCM streams, with honest gap handling.

A StreamWriter collects AUDIO_CHUNK payloads addressed by sample index. When
chunks are missing (dropped on the device or lost on the link) the hole is
filled with digital silence so timing stays correct, and the writer records
the gap so the run can be marked incomplete instead of pretending the file is
continuous.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class Gap:
    sample_index: int
    samples: int


@dataclass
class StreamWriter:
    name: str
    sample_rate: int = 16000
    channels: int = 1
    _chunks: List[bytes] = field(default_factory=list)
    _next_index: Optional[int] = None
    _first_index: Optional[int] = None
    gaps: List[Gap] = field(default_factory=list)
    samples_written: int = 0
    chunks_received: int = 0
    chunks_out_of_order: int = 0

    def add(self, sample_index: int, pcm: bytes) -> None:
        """pcm is little-endian int16, `channels` interleaved."""
        n = len(pcm) // (2 * self.channels)
        if n == 0:
            return
        self.chunks_received += 1
        if self._next_index is None:
            self._first_index = sample_index
            self._next_index = sample_index
        if sample_index > self._next_index:
            missing = sample_index - self._next_index
            self.gaps.append(Gap(self._next_index, missing))
            self._chunks.append(b"\x00" * (missing * 2 * self.channels))
            self.samples_written += missing
            self._next_index = sample_index
        elif sample_index < self._next_index:
            # Late or duplicate chunk; keep the file monotonic.
            self.chunks_out_of_order += 1
            return
        self._chunks.append(pcm[: n * 2 * self.channels])
        self.samples_written += n
        self._next_index += n

    @property
    def first_sample_index(self) -> int:
        return self._first_index or 0

    @property
    def duration_s(self) -> float:
        return self.samples_written / float(self.sample_rate)

    @property
    def complete(self) -> bool:
        return not self.gaps

    def pcm_bytes(self) -> bytes:
        return b"".join(self._chunks)

    def slice_samples(self, start: int, end: int) -> bytes:
        """Samples [start, end) relative to the first received sample index."""
        data = self.pcm_bytes()
        bps = 2 * self.channels
        s = max(0, start) * bps
        e = max(0, end) * bps
        return data[s:e]

    def write(self, path: str, start: int = 0, end: Optional[int] = None) -> dict:
        pcm = self.slice_samples(start, end if end is not None else self.samples_written)
        write_wav(path, pcm, self.sample_rate, self.channels)
        return {
            "file": path,
            "stream": self.name,
            "sample_rate": self.sample_rate,
            "channels": self.channels,
            "samples": len(pcm) // (2 * self.channels),
            "duration_s": round(len(pcm) / (2 * self.channels) / self.sample_rate, 3),
            "complete": self.complete,
            "gaps": [{"sample_index": g.sample_index, "samples": g.samples} for g in self.gaps],
            "chunks_received": self.chunks_received,
            "chunks_out_of_order": self.chunks_out_of_order,
            "first_sample_index": self.first_sample_index,
        }


def wav_header(data_bytes: int, sample_rate: int, channels: int, bits: int = 16) -> bytes:
    block_align = channels * bits // 8
    byte_rate = sample_rate * block_align
    return (b"RIFF" + struct.pack("<I", 36 + data_bytes) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, sample_rate, byte_rate,
                                    block_align, bits)
            + b"data" + struct.pack("<I", data_bytes))


def write_wav(path: str, pcm: bytes, sample_rate: int = 16000, channels: int = 1) -> None:
    with open(path, "wb") as f:
        f.write(wav_header(len(pcm), sample_rate, channels))
        f.write(pcm)


def read_wav(path: str):
    """Minimal PCM16 reader. Returns (pcm_bytes, sample_rate, channels)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    rate = channels = None
    pcm = b""
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8: pos + 8 + size]
        if cid == b"fmt ":
            fmt, channels, rate = struct.unpack_from("<HHI", body, 0)
            if fmt != 1:
                raise ValueError("only PCM WAV is supported")
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    return pcm, rate, channels


def interleave(*mono: bytes) -> bytes:
    """Interleave equal-length int16 mono byte strings into one multichannel string."""
    n = min(len(m) for m in mono) // 2
    ch = len(mono)
    out = bytearray(n * 2 * ch)
    for c, m in enumerate(mono):
        out[2 * c::2 * ch] = m[0:2 * n:2]
        out[2 * c + 1::2 * ch] = m[1:2 * n:2]
    return bytes(out)
