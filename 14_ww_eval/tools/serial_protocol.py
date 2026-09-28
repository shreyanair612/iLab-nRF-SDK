"""
Framed telemetry protocol, host side. Mirrors src/telemetry_protocol.h exactly.

Wire format (little-endian):
    0   2  magic 0xA5 0x5A
    2   1  version (1)
    3   1  type
    4   2  flags
    6   2  run_id
    8   2  stream_id
    10  4  seq
    14  4  timestamp (sample index for audio/events, uptime ms otherwise)
    18  2  payload_len
    20  n  payload
    20+n 2 crc16 (CRC-16/CCITT-FALSE over bytes [2 .. 20+n))

Plain text lines (device in text mode, or a human typing) are passed through
as ('text', line) events so the same parser works before and after the device
switches to binary framing.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator, List, Tuple, Union

MAGIC = b"\xA5\x5A"
VERSION = 1
HEADER = struct.Struct("<2sBBHHHIIH")
HEADER_LEN = HEADER.size  # 20
CRC_LEN = 2
MAX_PAYLOAD = 1024
MAX_PACKET = HEADER_LEN + MAX_PAYLOAD + CRC_LEN
STREAM_NONE = 0xFFFF

# device -> host
HELLO = 0x01
RUN_START = 0x02
RUN_CONFIG = 0x03
AUDIO_CHUNK = 0x04
AUDIO_GAP = 0x05
AUDIO_STATS = 0x06
WAKEWORD_EVENT = 0x07
PATH_METRICS = 0x08
RUN_END = 0x09
ERROR = 0x0A
LOG = 0x0B
ACK = 0x0C
STATUS = 0x0D
REPLAY_ACK = 0x0E
TEXT = 0x0F
# host -> device
COMMAND = 0x40
REPLAY_AUDIO = 0x41
REPLAY_END = 0x42

FLAG_INCOMPLETE = 1 << 0
FLAG_FINAL = 1 << 1
FLAG_REPLAY = 1 << 2

TYPE_NAMES = {
    HELLO: "HELLO", RUN_START: "RUN_START", RUN_CONFIG: "RUN_CONFIG",
    AUDIO_CHUNK: "AUDIO_CHUNK", AUDIO_GAP: "AUDIO_GAP", AUDIO_STATS: "AUDIO_STATS",
    WAKEWORD_EVENT: "WAKEWORD_EVENT", PATH_METRICS: "PATH_METRICS", RUN_END: "RUN_END",
    ERROR: "ERROR", LOG: "LOG", ACK: "ACK", STATUS: "STATUS", REPLAY_ACK: "REPLAY_ACK",
    TEXT: "TEXT", COMMAND: "COMMAND", REPLAY_AUDIO: "REPLAY_AUDIO", REPLAY_END: "REPLAY_END",
}

JSON_TYPES = {HELLO, RUN_START, RUN_CONFIG, AUDIO_GAP, AUDIO_STATS, WAKEWORD_EVENT,
              PATH_METRICS, RUN_END, STATUS, REPLAY_ACK}
TEXT_TYPES = {ERROR, LOG, ACK, TEXT, COMMAND}

# Stream ids, mirroring eval_config.h
STREAM_RAW_LEFT = 0
STREAM_RAW_RIGHT = 1
STREAM_RAW_CENTER = 2
STREAM_PATH_BASE = 16
PATH_NAMES = ["raw_left", "raw_right", "raw_lr_mix", "bf_lr", "raw_3ch", "bf_3ch"]
RAW_STREAM_NAMES = {STREAM_RAW_LEFT: "raw_left", STREAM_RAW_RIGHT: "raw_right",
                    STREAM_RAW_CENTER: "raw_center"}


def stream_name(stream_id: int) -> str:
    if stream_id in RAW_STREAM_NAMES:
        return RAW_STREAM_NAMES[stream_id]
    if STREAM_PATH_BASE <= stream_id < STREAM_PATH_BASE + len(PATH_NAMES):
        return PATH_NAMES[stream_id - STREAM_PATH_BASE]
    return f"stream_{stream_id}"


def path_stream_id(path_id: int) -> int:
    return STREAM_PATH_BASE + path_id


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE, bitwise, identical to tp_crc16 in the firmware."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Packet:
    type: int
    flags: int = 0
    run_id: int = 0
    stream_id: int = STREAM_NONE
    seq: int = 0
    timestamp: int = 0
    payload: bytes = b""

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, f"0x{self.type:02X}")

    @property
    def is_replay(self) -> bool:
        return bool(self.flags & FLAG_REPLAY)

    @property
    def is_final(self) -> bool:
        return bool(self.flags & FLAG_FINAL)

    def text(self) -> str:
        return self.payload.decode("utf-8", errors="replace")


def encode(type_: int, payload: bytes = b"", *, flags: int = 0, run_id: int = 0,
           stream_id: int = STREAM_NONE, seq: int = 0, timestamp: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)} > {MAX_PAYLOAD}")
    hdr = HEADER.pack(MAGIC, VERSION, type_, flags, run_id, stream_id, seq, timestamp, len(payload))
    body = hdr + payload
    return body + struct.pack("<H", crc16(body[2:]))


def encode_command(line: str) -> bytes:
    return encode(COMMAND, line.encode("utf-8"))


def encode_replay_audio(frame_index: int, interleaved_pcm: bytes, run_id: int = 0) -> bytes:
    return encode(REPLAY_AUDIO, interleaved_pcm, run_id=run_id, seq=frame_index, timestamp=frame_index)


def encode_replay_end(run_id: int = 0) -> bytes:
    return encode(REPLAY_END, b"", run_id=run_id)


Event = Union[Tuple[str, Packet], Tuple[str, str], Tuple[str, None]]


class StreamParser:
    """Incremental parser. feed() returns a list of events:
       ('packet', Packet) | ('text', str) | ('bad_crc', None) | ('resync', None)
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._text = bytearray()
        self.bad_crc = 0
        self.resyncs = 0
        self.packets = 0

    def feed(self, data: bytes) -> List[Event]:
        events: List[Event] = []
        self._buf += data
        while True:
            if not self._buf:
                break
            idx = self._buf.find(MAGIC)
            if idx == -1:
                # A trailing lone 0xA5 might be the start of a magic split across reads.
                keep = 1 if self._buf[-1:] == MAGIC[:1] else 0
                text_part = self._buf[: len(self._buf) - keep]
                if text_part:
                    events += self._text_events(bytes(text_part))
                del self._buf[: len(self._buf) - keep]
                break
            if idx > 0:
                events += self._text_events(bytes(self._buf[:idx]))
                del self._buf[:idx]
            if len(self._buf) < HEADER_LEN:
                break
            magic, ver, ptype, flags, run_id, sid, seq, ts, plen = HEADER.unpack(self._buf[:HEADER_LEN])
            if ver != VERSION or plen > MAX_PAYLOAD:
                self.resyncs += 1
                events.append(("resync", None))
                del self._buf[:2]
                continue
            total = HEADER_LEN + plen + CRC_LEN
            if len(self._buf) < total:
                break
            body = bytes(self._buf[:total])
            want = struct.unpack_from("<H", body, total - 2)[0]
            if crc16(body[2:total - 2]) != want:
                self.bad_crc += 1
                events.append(("bad_crc", None))
                del self._buf[:2]
                continue
            self.packets += 1
            events.append(("packet", Packet(ptype, flags, run_id, sid, seq, ts,
                                            body[HEADER_LEN:HEADER_LEN + plen])))
            del self._buf[:total]
        return events

    def _text_events(self, chunk: bytes) -> List[Event]:
        out: List[Event] = []
        self._text += chunk
        while True:
            nl = self._text.find(b"\n")
            if nl == -1:
                if len(self._text) > 4096:
                    self._text.clear()
                break
            line = self._text[:nl].rstrip(b"\r")
            del self._text[: nl + 1]
            if line:
                out.append(("text", line.decode("utf-8", errors="replace")))
        return out


@dataclass
class GapRecord:
    stream_id: int
    expected_seq: int
    got_seq: int
    frames_lost: int
    sample_index: int


@dataclass
class StreamTracker:
    """Detects missing sequence numbers per stream within one run."""
    expected: dict = field(default_factory=dict)
    gaps: List[GapRecord] = field(default_factory=list)

    def observe(self, pkt: Packet) -> GapRecord | None:
        key = (pkt.run_id, pkt.stream_id, pkt.is_replay)
        exp = self.expected.get(key)
        self.expected[key] = pkt.seq + 1
        if exp is None or pkt.seq == exp:
            return None
        if pkt.seq < exp:
            return None  # duplicate / reordered; ignore
        gap = GapRecord(pkt.stream_id, exp, pkt.seq, pkt.seq - exp, pkt.timestamp)
        self.gaps.append(gap)
        return gap

    def reset(self) -> None:
        self.expected.clear()
        self.gaps.clear()


def iter_frames(pcm_l: bytes, pcm_r: bytes, frame_samples: int = 256) -> Iterator[Tuple[int, bytes]]:
    """Interleave two mono int16 byte strings into (frame_index, stereo_bytes) frames
    for replay. The last partial frame is zero-padded to a full frame."""
    n = min(len(pcm_l), len(pcm_r)) // 2
    frames = (n + frame_samples - 1) // frame_samples
    for i in range(frames):
        s = i * frame_samples
        e = min(s + frame_samples, n)
        l = pcm_l[2 * s: 2 * e]
        r = pcm_r[2 * s: 2 * e]
        pad = (frame_samples - (e - s)) * 2
        l += b"\x00" * pad
        r += b"\x00" * pad
        out = bytearray(frame_samples * 4)
        out[0::4] = l[0::2]
        out[1::4] = l[1::2]
        out[2::4] = r[0::2]
        out[3::4] = r[1::2]
        yield i, bytes(out)
