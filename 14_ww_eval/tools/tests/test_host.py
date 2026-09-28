"""
Host-side tests. Run with either:
    python3 -m unittest discover -s tools/tests -v
    pytest tools/tests

The DSP cross-check reads tests_host/vectors.json, produced by
`make -C tests_host`, so the Python reference is compared against CRCs the
firmware's own C objects computed.
"""
import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
PROJECT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import dsp_reference as dsp  # noqa: E402
import run_storage  # noqa: E402
import serial_protocol as sp  # noqa: E402
import wav_writer  # noqa: E402


class ProtocolTests(unittest.TestCase):
    def test_crc16_check_value(self):
        self.assertEqual(sp.crc16(b"123456789"), 0x29B1)

    def test_roundtrip(self):
        pcm = struct.pack("<256h", *[(i * 37 - 4000) for i in range(256)])
        raw = sp.encode(sp.AUDIO_CHUNK, pcm, flags=sp.FLAG_FINAL, run_id=7, stream_id=1,
                        seq=123456, timestamp=987654)
        self.assertEqual(len(raw), sp.HEADER_LEN + 512 + sp.CRC_LEN)
        p = sp.StreamParser()
        events = []
        for i in range(0, len(raw), 7):        # arbitrary chunking
            events += p.feed(raw[i:i + 7])
        self.assertEqual(len(events), 1)
        kind, pkt = events[0]
        self.assertEqual(kind, "packet")
        self.assertEqual((pkt.type, pkt.run_id, pkt.stream_id, pkt.seq, pkt.timestamp),
                         (sp.AUDIO_CHUNK, 7, 1, 123456, 987654))
        self.assertEqual(pkt.payload, pcm)
        self.assertTrue(pkt.is_final)

    def test_header_layout_matches_c(self):
        raw = sp.encode(sp.HELLO, b"x", flags=0x0004, run_id=0x0102, stream_id=0xFFFF,
                        seq=0x11223344, timestamp=0x55667788)
        self.assertEqual(raw[0:2], b"\xA5\x5A")
        self.assertEqual(raw[2], 1)
        self.assertEqual(raw[3], sp.HELLO)
        self.assertEqual(raw[4:6], b"\x04\x00")
        self.assertEqual(raw[6:8], b"\x02\x01")
        self.assertEqual(raw[8:10], b"\xFF\xFF")
        self.assertEqual(raw[10:14], b"\x44\x33\x22\x11")
        self.assertEqual(raw[14:18], b"\x88\x77\x66\x55")
        self.assertEqual(raw[18:20], b"\x01\x00")

    def test_bad_crc_and_recovery(self):
        good = sp.encode(sp.STATUS, b'{"a":1}')
        bad = bytearray(good)
        bad[25] ^= 0x55
        p = sp.StreamParser()
        ev = p.feed(bytes(bad) + good)
        kinds = [k for k, _ in ev]
        self.assertIn("bad_crc", kinds)
        self.assertEqual(kinds[-1], "packet")
        self.assertEqual(p.bad_crc, 1)

    def test_text_passthrough(self):
        p = sp.StreamParser()
        ev = p.feed(b"ok: hello\r\nsome text\n")
        self.assertEqual(ev, [("text", "ok: hello"), ("text", "some text")])
        # text before a packet, packet, then text split across feeds
        pkt = sp.encode(sp.LOG, b"line")
        ev = p.feed(b"pre\n" + pkt + b"po")
        ev += p.feed(b"st\n")
        self.assertEqual(ev[0], ("text", "pre"))
        self.assertEqual(ev[1][0], "packet")
        self.assertEqual(ev[2], ("text", "post"))

    def test_gap_tracker(self):
        t = sp.StreamTracker()
        mk = lambda seq, ts: sp.Packet(sp.AUDIO_CHUNK, 0, 1, 0, seq, ts, b"")
        self.assertIsNone(t.observe(mk(0, 0)))
        self.assertIsNone(t.observe(mk(1, 256)))
        gap = t.observe(mk(4, 1024))
        self.assertIsNotNone(gap)
        self.assertEqual((gap.expected_seq, gap.got_seq, gap.frames_lost), (2, 4, 2))
        self.assertIsNone(t.observe(mk(5, 1280)))
        self.assertEqual(len(t.gaps), 1)

    def test_replay_frames_interleave(self):
        l = struct.pack("<300h", *range(300))
        r = struct.pack("<300h", *[-x for x in range(300)])
        frames = list(sp.iter_frames(l, r, 256))
        self.assertEqual(len(frames), 2)
        idx, data = frames[0]
        self.assertEqual(idx, 0)
        self.assertEqual(len(data), 256 * 4)
        vals = struct.unpack("<512h", data)
        self.assertEqual(vals[0:4], (0, 0, 1, -1))
        # last frame is zero padded
        idx, data = frames[1]
        vals = struct.unpack("<512h", data)
        self.assertEqual(vals[2 * 43:2 * 43 + 2], (299, -299))
        self.assertEqual(vals[2 * 43 + 2:2 * 43 + 6], (0, 0, 0, 0))


class WavTests(unittest.TestCase):
    def test_write_read_roundtrip(self):
        pcm = struct.pack("<8h", 1, -1, 32767, -32768, 0, 5, 6, 7)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.wav")
            wav_writer.write_wav(p, pcm, 16000, 1)
            data, rate, ch = wav_writer.read_wav(p)
            self.assertEqual((data, rate, ch), (pcm, 16000, 1))
            with open(p, "rb") as f:
                head = f.read(44)
            self.assertEqual(head[:4], b"RIFF")
            self.assertEqual(struct.unpack_from("<I", head, 40)[0], len(pcm))

    def test_stream_writer_fills_gaps(self):
        w = wav_writer.StreamWriter("raw_left")
        w.add(0, b"\x01\x00" * 256)
        w.add(256, b"\x02\x00" * 256)
        w.add(1024, b"\x03\x00" * 256)     # 512..1023 missing (two frames)
        self.assertFalse(w.complete)
        self.assertEqual(len(w.gaps), 1)
        self.assertEqual((w.gaps[0].sample_index, w.gaps[0].samples), (512, 512))
        self.assertEqual(w.samples_written, 1280)
        pcm = w.pcm_bytes()
        self.assertEqual(pcm[512 * 2:512 * 2 + 2], b"\x00\x00")
        self.assertEqual(pcm[1024 * 2:1024 * 2 + 2], b"\x03\x00")
        with tempfile.TemporaryDirectory() as d:
            info = w.write(os.path.join(d, "x.wav"))
            self.assertFalse(info["complete"])
            self.assertEqual(info["samples"], 1280)

    def test_interleave(self):
        l = struct.pack("<3h", 1, 2, 3)
        r = struct.pack("<3h", -1, -2, -3)
        st = wav_writer.interleave(l, r)
        self.assertEqual(struct.unpack("<6h", st), (1, -1, 2, -2, 3, -3))


class DspReferenceTests(unittest.TestCase):
    def test_tdiv_and_mix(self):
        self.assertEqual(dsp.tdiv(-3, 2), -1)
        self.assertEqual(dsp.tdiv(3, 2), 1)
        self.assertEqual(dsp.lr_mix([32767], [32767]), [32766])
        self.assertEqual(dsp.lr_mix([-32768], [-32768]), [-32768])
        self.assertEqual(dsp.lr_mix([-3], [5]), [1])

    def test_delay_moves_impulse_and_history(self):
        l = [0] * 12
        l[0] = 20000
        r = [0] * 12
        out, clips = dsp.delay_and_sum([l, r], [2, 0], frame=6)
        self.assertEqual(out[0:2], [0, 0])
        self.assertTrue(9000 < out[2] <= 10000)
        # impulse at end of frame 1 shows at index 1 of frame 2 (frame=6, delay=2)
        l2 = [0] * 12
        l2[5] = 20000
        out, _ = dsp.delay_and_sum([l2, r], [2, 0], frame=6)
        self.assertEqual(out[5], 0)
        self.assertTrue(out[7] > 9000)

    def test_matches_firmware_vectors(self):
        vec_path = os.path.join(PROJECT, "tests_host", "vectors.json")
        if not os.path.isfile(vec_path):
            self.skipTest("run `make -C tests_host` first to produce vectors.json")
        with open(vec_path) as f:
            vec = json.load(f)
        seed = vec["seed"]
        n = vec["frames"] * vec["frame"]
        state = seed & 0xFFFFFFFF
        l, r = [], []

        def lcg():
            nonlocal state
            state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
            return state

        for _ in range(n):
            l.append(struct.unpack("<h", struct.pack("<H", lcg() >> 16))[0])
            r.append(struct.unpack("<h", struct.pack("<H", lcg() >> 16))[0])
        lb, rb = dsp.samples_to_bytes(l), dsp.samples_to_bytes(r)
        cfg = {"frame": vec["frame"], "paths": [
            {"name": "bf_lr", "dl": vec["dl"], "dr": vec["dr"], "gain": vec["gain"]}]}
        for path in ("raw_left", "raw_right", "raw_lr_mix", "bf_lr"):
            pcm, info = dsp.build_path(path, lb, rb, None, cfg)
            self.assertEqual(info["crc32"], vec["crc"][path], f"{path} CRC mismatch vs firmware")
        _, info = dsp.build_path("bf_lr", lb, rb, None, cfg)
        self.assertEqual(info["clips"], vec["bf_clips"])


class StorageTests(unittest.TestCase):
    def test_run_folder_lifecycle(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = {"label": "angle 45 test", "angle_deg": 45, "distance_cm": 100,
                   "preset": "raw_vs_bf_lr",
                   "paths": [{"name": "bf_lr", "enabled": 1, "dl": 3, "dr": 0}]}
            rf = run_storage.RunFolder(d, cfg, when=0)
            self.assertIn("angle045_distance100cm_angle_45_test", rf.name)
            rf.add_event("WAKEWORD_EVENT", {"path": 3, "sample": 1234})
            rf.add_log("LOG hello")
            rf.add_metrics({"run_id": 1, "replay": 0, "sample_index": 0, "path": 3,
                            "name": "bf_lr", "det": 1})
            rf.add_stats({"run_id": 1, "replay": 0, "sample_index": 0, "stream": 0,
                          "name": "raw_left", "rms": 10, "peak": 20, "clips": 0})
            rf.warn("dropped frames")
            rf.warn("dropped frames")
            rf.write_summary([{"path": 3, "name": "bf_lr", "detections": 2}])
            rf.close("complete")
            meta = run_storage.load_metadata(rf.path)
            self.assertEqual(meta["completion"], "complete")
            self.assertEqual(meta["warnings"], ["dropped frames"])
            ev = run_storage.load_events(rf.path)
            self.assertEqual(ev[0]["type"], "WAKEWORD_EVENT")
            runs = run_storage.list_runs(d)
            self.assertEqual(len(runs), 1)
            self.assertEqual(runs[0]["angle_deg"], 45)
            self.assertEqual(runs[0]["detections"], {"bf_lr": "2"})
            self.assertEqual(runs[0]["bf_lr"], "3/0")
            for name in ("events.jsonl", "device_serial_log.txt", "metrics_timeseries.csv",
                         "audio_stats.csv", "run_summary.csv", "run_metadata.json"):
                self.assertTrue(os.path.isfile(os.path.join(rf.path, name)), name)


if __name__ == "__main__":
    unittest.main()
