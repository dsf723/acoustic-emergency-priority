"""Offline synthetic tests for the PC training workflow, not real accuracy tests.

Run: python -I tools/test_train_ambulance.py
All fixtures and models live in temporary directories and are removed afterwards.
No hardware, serial port, JTAG, or network is accessed.
"""
import contextlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

import joblib
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import train_ambulance as baseline


def fixture_session(root, label, source_id, *, name=None, review="approved", status="complete", no_raw=False):
    folder = Path(root) / (name or source_id)
    folder.mkdir(parents=True)
    metadata = {
        "schema_version": 1, "session_id": folder.name, "label": label, "source_id": source_id,
        "status": status, "label_provenance": "synthetic_test_fixture_not_real_audio",
        "capture": {"actual_sample_rate_hz": 48828.125, "resampled": False,
                    "channel_map": {"outer_ring": [0, 1, 2, 3, 4, 5], "center": 7, "unused_zero": [6]}},
        "quality": {"validation_status": "passed" if status == "complete" else "failed", "content_review_status": review},
        "files": {"raw": {"file": "raw.npz"}},
    }
    if not no_raw:
        size = baseline.BLOCKS * baseline.FRAMES
        t = np.arange(size) / baseline.SOURCE_FS
        # Different originals have different deterministic synthetic waveforms.
        seed = sum(source_id.encode("utf-8"))
        generator = np.random.default_rng(seed)
        if label == "ambulance":
            instantaneous = 800 + (seed % 80) + 300 * np.sin(2 * np.pi * (0.7 + (seed % 5) * 0.08) * t)
            x = 0.08 * np.sin(2 * np.pi * np.cumsum(instantaneous) / baseline.SOURCE_FS)
            x += 0.001 * generator.standard_normal(size)
        else:
            # Broadband/modulated noise is an easy workflow fixture, NOT a real
            # police/fire/horn hard negative or ambulance discrimination claim.
            x = 0.025 * generator.standard_normal(size) * (0.7 + 0.3 * np.sin(2 * np.pi * 3 * t))
        channel = np.rint(x * (1 << 23)).astype(np.int32)
        samples = np.zeros((baseline.BLOCKS, baseline.FRAMES, baseline.CHANNELS), dtype=np.int32)
        for index in [0, 1, 2, 3, 4, 5, 7]:
            samples[:, :, index] = channel.reshape(baseline.BLOCKS, baseline.FRAMES)
        np.savez(folder / "raw.npz", samples=samples, blkno=np.arange(baseline.BLOCKS, dtype=np.uint32), rec_idx=baseline.BLOCKS)
    (folder / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
    return folder


def fake_sessions():
    return [baseline.Session(Path(f"{label}_{index}/metadata.json"), Path("unused.npz"), f"{label}_{index}", label, {}, str(index))
            for label in ("ambulance", "speech") for index in range(5)]


class TrainingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_unapproved_and_failed_rejected_without_opening_audio(self):
        fixture_session(self.root, "ambulance", "positive_bad", review="needs_rerecording", no_raw=True)
        fixture_session(self.root, "background", "failed_background", review="not_reviewed", status="failed", no_raw=True)
        sessions, inventory = baseline.inspect_dataset(self.root)
        self.assertEqual(sessions, [])
        self.assertEqual(inventory["accepted_sessions"], 0)
        self.assertEqual(inventory["content_review_status_counts"]["needs_rerecording"], 1)
        self.assertEqual(inventory["missing_independent_sources"], {"ambulance": 3, "non_target": 3})
        output = self.root / "must_not_exist"
        with self.assertRaisesRegex(baseline.DatasetError, "missing 3"):
            baseline.train(sessions, inventory, output_dir=output)
        self.assertFalse(output.exists())

    def test_same_original_source_conflicting_labels_refused(self):
        fixture_session(self.root, "ambulance", "original_A", name="take1", review="not_reviewed", no_raw=True)
        fixture_session(self.root, "police", " ORIGINAL_a ", name="take2", review="not_reviewed", no_raw=True)
        _, inventory = baseline.inspect_dataset(self.root)
        self.assertTrue(any("Conflicting labels" in message for message in inventory["fatal_errors"]))
        with self.assertRaisesRegex(baseline.DatasetError, "Conflicting labels"):
            baseline.require_ready(inventory)

    def test_group_split_is_reproducible_stratified_and_never_window_random(self):
        sessions = fake_sessions()
        mapping = baseline.split_sources(sessions, seed=42)
        self.assertEqual(mapping, baseline.split_sources(list(reversed(sessions)), seed=42))
        # A re-record/augmentation keeps its original source_id and assignment.
        duplicate = baseline.Session(Path("rerecord/metadata.json"), Path("unused.npz"), sessions[0].source_id, sessions[0].label, {}, "other")
        self.assertEqual(mapping, baseline.split_sources(sessions + [duplicate], seed=42))
        for split in baseline.SPLITS:
            targets = {session.target for session in sessions if mapping[session.source_id] == split}
            self.assertEqual(targets, {0, 1})
        split_groups = [{source for source, value in mapping.items() if value == split} for split in baseline.SPLITS]
        for index, group in enumerate(split_groups):
            for other in split_groups[index + 1:]:
                self.assertFalse(group & other)

    def test_development_sources_train_only_and_need_two_unseen_per_class(self):
        sessions = fake_sessions()
        development = {"ambulance_0", "speech_0"}
        mapping = baseline.split_sources(sessions, seed=42, development_sources=development)
        self.assertTrue(all(mapping[source] == "train" for source in development))
        self.assertTrue(all(split in {"validation", "test"} for source, split in mapping.items() if source not in development))
        too_many_development = {f"ambulance_{index}" for index in range(4)}
        with self.assertRaisesRegex(baseline.DatasetError, "two non-development"):
            baseline.split_sources(sessions, development_sources=too_many_development)

    def test_conflicting_reserved_roles_are_refused_even_in_excluded_sessions(self):
        first = fixture_session(self.root, "ambulance", "same_original", name="dev", review="not_reviewed", no_raw=True)
        second = fixture_session(self.root, "ambulance", "same_original", name="requested_test", review="not_reviewed", no_raw=True)
        for folder, update in ((first, {"source_usage": "development"}), (second, {"split_role": "test"})):
            metadata = json.loads((folder / "metadata.json").read_text())
            metadata.update(update)
            (folder / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
        _, inventory = baseline.inspect_dataset(self.root)
        self.assertEqual(inventory["development_source_groups"], ["same_original"])
        self.assertTrue(any("Conflicting reserved split roles" in message for message in inventory["fatal_errors"]))
        with self.assertRaisesRegex(baseline.DatasetError, "Conflicting reserved split roles"):
            baseline.require_ready(inventory)

    def test_threshold_selection_uses_supplied_validation_only(self):
        labels = np.array([0, 0, 1, 1])
        scores = np.array([0.1, 0.3, 0.7, 0.9])
        threshold = baseline.select_threshold(labels, scores)
        self.assertEqual(threshold, 0.7)
        self.assertEqual(baseline.metrics(labels, scores, threshold)["false_positive_windows"], 0)

    def test_feature_configuration_and_silent_numeric_stability(self):
        self.assertEqual(baseline.SOURCE_FS * baseline.CONFIG["resample_up"] / baseline.CONFIG["resample_down"], 8000)
        vector = baseline.extract_window(np.zeros(16000))
        self.assertEqual(vector.shape, (len(baseline.FEATURE_NAMES),))
        self.assertTrue(np.isfinite(vector).all())
        self.assertEqual(len(set(baseline.FEATURE_NAMES)), len(baseline.FEATURE_NAMES))

    def test_raw_gaps_excluded_even_if_human_approved(self):
        folder = fixture_session(self.root, "ambulance", "gap_source")
        with np.load(folder / "raw.npz") as source:
            samples = source["samples"]
            bn = source["blkno"]
        bn[50:] += 1
        np.savez(folder / "raw.npz", samples=samples, blkno=bn, rec_idx=baseline.BLOCKS)
        sessions, inventory = baseline.inspect_dataset(self.root)
        self.assertFalse(sessions)
        self.assertIn("block-index gaps", inventory["sessions"][0]["reasons"][0])

    def test_same_raw_copied_under_two_source_ids_is_fatal(self):
        first = fixture_session(self.root, "ambulance", "first_source")
        second = fixture_session(self.root, "ambulance", "second_source", no_raw=True)
        shutil.copy2(first / "raw.npz", second / "raw.npz")
        _, inventory = baseline.inspect_dataset(self.root)
        self.assertTrue(any("Identical raw NPZ" in message for message in inventory["fatal_errors"]))

    def test_full_synthetic_training_evaluation_persistence_and_cli_dry_run(self):
        for label in ("ambulance", "speech"):
            for index in range(3):
                fixture_session(self.root / "dataset", label, f"{label}_synthetic_{index}")
        # A declaration on an excluded historical take still locks the approved
        # re-recording of the same original source to train.
        marker = fixture_session(self.root / "dataset", "ambulance", "ambulance_synthetic_0", name="excluded_development_marker", review="not_reviewed", no_raw=True)
        marker_metadata = json.loads((marker / "metadata.json").read_text())
        marker_metadata["source_usage"] = "development"
        (marker / "metadata.json").write_text(json.dumps(marker_metadata), encoding="utf-8")
        negative_metadata_path = self.root / "dataset/speech_synthetic_0/metadata.json"
        negative_metadata = json.loads(negative_metadata_path.read_text())
        negative_metadata["split_role"] = "train"
        negative_metadata_path.write_text(json.dumps(negative_metadata), encoding="utf-8")
        sessions, inventory = baseline.inspect_dataset(self.root / "dataset")
        self.assertEqual(inventory["accepted_sessions"], 6)
        self.assertTrue(inventory["ready_for_training"])
        self.assertEqual(inventory["development_source_groups"], ["ambulance_synthetic_0", "speech_synthetic_0"])
        with contextlib.redirect_stdout(io.StringIO()):
            result = baseline.main(["--dataset-root", str(self.root / "dataset"), "--dry-run", "--output-dir", str(self.root / "dry_no_model")])
        self.assertEqual(result, 0)
        self.assertFalse((self.root / "dry_no_model").exists())
        with patch.object(baseline, "select_threshold", wraps=baseline.select_threshold) as selector:
            output, report = baseline.train(sessions, inventory, seed=77, output_dir=self.root / "synthetic_model_only")
        self.assertEqual(selector.call_count, 1)
        self.assertEqual(len(selector.call_args.args[0]), report["splits"]["validation"]["windows"])
        self.assertEqual(report["threshold_selected_on"], "validation")
        self.assertEqual(report["source_split_manifest"]["ambulance_synthetic_0"], "train")
        self.assertEqual(report["source_split_details"]["speech_synthetic_0"], {"split": "train", "source_usage": "development"})
        self.assertFalse(report["test_used_for_threshold"])
        self.assertFalse(report["detection_latency"]["measured"])
        self.assertFalse(report["labels"]["machine_verified"])
        self.assertFalse(report["deployment"]["arm_or_fpga_model_deployed"])
        self.assertEqual(report["splits"]["test"]["windows"], 30)
        self.assertEqual(report["feature_count"], len(baseline.FEATURE_NAMES))
        bundle = joblib.load(output / "model.joblib")
        self.assertEqual(bundle["tool_version"], baseline.VERSION)
        self.assertEqual(bundle["threshold"], report["threshold"])
        saved = json.loads((output / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["source_split_manifest"], report["source_split_manifest"])
        # StandardScaler was fitted on training windows only.
        self.assertEqual(int(bundle["estimator"].named_steps["standardscaler"].n_samples_seen_), report["splits"]["train"]["windows"])
        for split in baseline.SPLITS:
            score = report["splits"][split]
            self.assertEqual(np.asarray(score["confusion_matrix"]).sum(), score["windows"])
            self.assertTrue(0 <= score["precision"] <= 1)
            self.assertTrue(0 <= score["recall"] <= 1)
        # Intentionally no assertion of high accuracy: fixtures prove plumbing,
        # not performance on ambulance/police/fire/music recordings.
        extra_development = self.root / "dataset/ambulance_synthetic_1/metadata.json"
        changed = json.loads(extra_development.read_text())
        changed["source_usage"] = "development"
        extra_development.write_text(json.dumps(changed), encoding="utf-8")
        changed_sessions, changed_inventory = baseline.inspect_dataset(self.root / "dataset")
        self.assertEqual(changed_inventory["missing_independent_sources"]["ambulance"], 0)
        self.assertEqual(changed_inventory["missing_non_development_sources"]["ambulance"], 1)
        self.assertFalse(changed_inventory["ready_for_training"])
        refused_output = self.root / "missing_unseen_no_model"
        with self.assertRaisesRegex(baseline.DatasetError, "non-development"):
            baseline.train(changed_sessions, changed_inventory, output_dir=refused_output)
        self.assertFalse(refused_output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
