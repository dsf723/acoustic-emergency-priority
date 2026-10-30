"""Offline tests only: synthetic NPZ and stubbed subprocess/JTAG responses.

No test opens a serial port, connects to JTAG, or creates persistent recordings.
Run: python -I tools/test_capture_labelled.py
"""
import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_labelled as capture


def synthetic_samples():
    """PCM boundary pattern, NOT a siren recording or a detector test."""
    pcm = np.zeros((capture.BLOCKS, capture.FRAMES, capture.CHANNELS), dtype=np.int32)
    pcm[0, 0, :6] = [-(1 << 23), -65536, -1, 0, 65536, (1 << 23) - 1]
    pcm[0, 1, :6] = [7, -7, 256, -256, 500000, -500000]
    pcm[-1, -1, :6] = [1, 2, 3, 4, 5, 6]
    pcm[0, 0, 7] = 42
    return pcm


def save_synthetic(path, *, missing=False, gap=False, outside=False):
    pcm = synthetic_samples()
    bn = np.arange(capture.BLOCKS, dtype=np.uint32) + np.uint32(310)
    if gap:
        bn[100:] += 1
    if outside:
        pcm[0, 0, 0] = 1 << 23
    if missing:
        pcm, bn = pcm[:-1], bn[:-1]
    np.savez_compressed(path, samples=pcm, blkno=bn, rec_idx=np.array(len(bn)))


def args_for(root):
    return argparse.Namespace(label="ambulance", source_id="test_source_A", source_description="synthetic fixture; not real audio",
                              distance_m=1.0, angle_deg=0.0, volume="test only", room="test",
                              notes="synthetic, no real hardware", dataset_root=Path(root) / "dataset",
                              timeout_seconds=90, command_timeout=120, poll_seconds=0.01, no_wav=False)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_npz_integrity_and_pcm24_six_channel_export(self):
        raw = self.root / "synthetic.npz"
        save_synthetic(raw)
        pcm, quality = capture.validate_recording(raw)
        self.assertEqual(quality["recorded_blocks"], 768)
        self.assertTrue(quality["block_indices_contiguous"])
        self.assertFalse(quality["sample_continuity_proven_from_npz"])
        wav = self.root / "synthetic.wav"
        info = capture.export_outer_wav(wav, pcm)
        self.assertEqual(info["actual_sample_rate_hz"], 48828.125)
        self.assertFalse(info["resampled"])
        with wave.open(str(wav), "rb") as stream:
            self.assertEqual((stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getnframes()), (6, 3, 48828, 768 * 1024))
            packed = np.frombuffer(stream.readframes(2), dtype=np.uint8).reshape(2, 6, 3).astype(np.uint32)
        value = packed[:, :, 0] | (packed[:, :, 1] << 8) | (packed[:, :, 2] << 16)
        signed = (value.astype(np.int32) << 8) >> 8
        np.testing.assert_array_equal(signed, pcm[0, :2, :6])

    def test_integrity_rejects_incomplete_gaps_and_pcm_overflow(self):
        for options in ({"missing": True}, {"gap": True}, {"outside": True}):
            with self.subTest(options=options):
                raw = self.root / "bad.npz"
                save_synthetic(raw, **options)
                with self.assertRaises(capture.CaptureError):
                    capture.validate_recording(raw)

    def test_uint32_block_counter_wrap_is_contiguous(self):
        raw = self.root / "wrap.npz"
        bn = (np.arange(capture.BLOCKS, dtype=np.uint64) + (1 << 32) - 10).astype(np.uint32)
        np.savez_compressed(raw, samples=synthetic_samples(), blkno=bn, rec_idx=capture.BLOCKS)
        _, quality = capture.validate_recording(raw)
        self.assertTrue(quality["block_indices_contiguous"])

    def test_status_parser_gives_useful_jtag_error(self):
        with self.assertRaisesRegex(capture.CaptureError, "JTAG"):
            capture.parse_status("No targets found")

    def run_stubbed_session(self, *, gap=False, counter_error=False, missing_counters=False, partial=False, timeout=False):
        commands = []
        count_reads = 0
        status_reads = 0
        entered = False

        def prompt(_):
            nonlocal entered
            self.assertFalse(commands, "No hardware command before operator Enter")
            entered = True
            return ""

        def run(command, **kwargs):
            nonlocal count_reads, status_reads
            self.assertTrue(entered)
            self.assertTrue(kwargs["timeout"] > 0)
            commands.append(command)
            if "-c" in command:
                count_reads += 1
                values = {name: None if missing_counters else 0 for name in capture.COUNTERS}
                if counter_error and count_reads > 1:
                    values["capture_seq_errors"] = 1
                out = "CAPTURE_COUNTERS " + json.dumps(values) + "\n"
            elif command[-1] == "--continuous":
                out = "continuous recording started (about 16 s)\n"
            elif command[-1] == "--status":
                status_reads += 1
                if timeout:
                    out = "mode=1 recorded_blocks=0 left=768 frozen=0\n"
                elif status_reads == 1:
                    out = "mode=1 recorded_blocks=100 left=668 frozen=0\n"
                elif partial:
                    out = "mode=1 recorded_blocks=10 left=758 frozen=1\n"
                else:
                    out = "mode=1 recorded_blocks=768 left=0 frozen=1\n"
            else:
                save_synthetic(Path(command[-1]), gap=gap)
                out = "saved synthetic fixture only\n"
            return subprocess.CompletedProcess(command, 0, out, "")

        tick = 0.0

        def clock():
            nonlocal tick
            tick += 50 if timeout else 0.1
            return tick

        with patch.object(capture.subprocess, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            session, ok = capture.capture(args_for(self.root), prompt=prompt, sleep=lambda _: None, clock=clock)
        metadata = json.loads((session / "metadata.json").read_text(encoding="utf-8"))
        return session, ok, metadata, commands

    def test_subprocess_stub_full_capture_and_metadata(self):
        session, ok, metadata, commands = self.run_stubbed_session()
        self.assertTrue(ok)
        self.assertEqual(metadata["status"], "complete")
        self.assertEqual(metadata["label_provenance"], "user_provided_unverified")
        self.assertFalse(metadata["label_machine_verified"])
        self.assertEqual(metadata["capture"]["actual_sample_rate_hz"], 48828.125)
        self.assertEqual(metadata["capture"]["channel_map"]["center"], 7)
        self.assertEqual(metadata["operator_metadata"]["angle_deg"], 0)
        self.assertTrue(metadata["quality"]["pipeline_counters_available"])
        self.assertEqual(metadata["quality"]["pipeline_counter_delta"]["capture_seq_errors"], 0)
        self.assertTrue((session / "outer6_pcm24.wav").is_file())
        self.assertTrue(any(command[-1] == "--continuous" for command in commands))
        self.assertFalse(any("--arm" in command for command in commands))
        self.assertEqual(len(metadata["files"]["raw"]["sha256"]), 64)

    def test_missing_optional_counters_are_explicit(self):
        _, ok, metadata, _ = self.run_stubbed_session(missing_counters=True)
        self.assertTrue(ok)
        self.assertFalse(metadata["quality"]["pipeline_counters_available"])
        self.assertIn("absent", metadata["quality"]["pipeline_counters_note"])

    def test_pipeline_error_and_gap_never_mark_complete(self):
        for options in ({"counter_error": True}, {"gap": True}, {"partial": True}, {"timeout": True}):
            with self.subTest(options=options):
                session, ok, metadata, _ = self.run_stubbed_session(**options)
                self.assertFalse(ok)
                self.assertEqual(metadata["status"], "failed")
                self.assertEqual(metadata["quality"]["validation_status"], "failed")
                self.assertNotIn("wav", metadata["files"])
                self.assertFalse((session / "outer6_pcm24.wav").exists())

    def test_unique_sessions_do_not_overwrite(self):
        first = capture.create_session(self.root, "music", "one/source")
        (first / "keep.txt").write_text("preserve", encoding="utf-8")
        second = capture.create_session(self.root, "music", "one/source")
        self.assertNotEqual(first, second)
        self.assertEqual((first / "keep.txt").read_text(encoding="utf-8"), "preserve")
        self.assertEqual(first.parent, second.parent)

    def test_subprocess_timeout_is_bounded_and_logged(self):
        session = capture.create_session(self.root, "background", "test")
        backend = capture.RecDumpBackend(session, 0.1)
        with patch.object(capture.subprocess, "run", side_effect=subprocess.TimeoutExpired("stub", 0.1)):
            with self.assertRaisesRegex(capture.CaptureError, "timed out"):
                backend.command("--status")
        self.assertIn("Timed out", (session / "commands.jsonl").read_text(encoding="utf-8"))

    def test_operator_cancellation_is_not_valid(self):
        def cancelled(_):
            raise KeyboardInterrupt()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            session, ok = capture.capture(args_for(self.root), prompt=cancelled)
        self.assertFalse(ok)
        self.assertEqual(json.loads((session / "metadata.json").read_text(encoding="utf-8"))["status"], "cancelled")


if __name__ == "__main__":
    unittest.main(verbosity=2)
