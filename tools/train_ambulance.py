"""PC-only ambulance/non-target baseline, version ambulance_logmel_lr_v1.

Inventory/review gate (does not create a model):
  python -I tools/train_ambulance.py --dry-run
Train after operator listening approval and enough independent source groups:
  python -I tools/train_ambulance.py --output-dir build/ambulance_model_run1

Only status=complete AND quality.content_review_status=approved sessions enter
training. Approval records a human judgement; this tool never approves a label.
All recordings/segments/replays/augmentations of one original clip MUST share
source_id. Splitting windows at random is forbidden. At least three independent
source_id groups per binary class are required to populate train/validation/test.
Mark any source used for rule/feature/debug decisions source_usage="development"
(or split_role="train") in ANY session of that source. The whole source is then
train-only; at least two other, non-development sources per class are mandatory.
This is a small supervised baseline, not a pretrained ambulance recognizer.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import uuid

import joblib
import numpy as np
import scipy
from scipy import signal
import sklearn
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import confusion_matrix, precision_recall_fscore_support
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler


ROOT = Path(__file__).resolve().parent.parent
VERSION = "ambulance_logmel_lr_v1"
LABELS = ("ambulance", "police", "fire", "horn", "speech", "music", "clap", "background")
SOURCE_FS = 48828.125
TARGET_FS = 8000
BLOCKS, FRAMES, CHANNELS = 768, 1024, 8
MIN_SOURCES_PER_CLASS = 3
MIN_NON_DEVELOPMENT_SOURCES_PER_CLASS = 2
SPLIT_POLICY_VERSION = "development_locked_v1"
SPLITS = ("train", "validation", "test")
CONFIG = {
    "tool_version": VERSION,
    "raw_input": "raw.npz signed PCM24 stored in int32; outer-ring channel ch0 only",
    "source_sample_rate_hz": SOURCE_FS,
    "wav_header_sample_rate_hz": 48828,
    "wav_header_is_not_used_for_resampling": True,
    "preprocessing": "subtract whole-recording ch0 mean, divide by 2**23; no per-class/whole-dataset normalization",
    "resample_up": 512, "resample_down": 3125, "resampled_sample_rate_hz": TARGET_FS,
    "resample_filter": "scipy.signal.resample_poly, Kaiser beta=5.0",
    "window_seconds": 2.0, "window_hop_seconds": 1.0,
    "frame_seconds": 0.025, "frame_hop_seconds": 0.010, "fft_size": 256,
    "mel_bins": 32, "mel_min_hz": 100, "mel_max_hz": 3500,
    "log_mel": "natural log of each frame's mel energies divided by summed mel energy; floor 1e-12",
    "aggregations": "per-mel mean/std/mean absolute frame difference; spectral ridge range/variation, concentration, flatness, modulation and envelope rhythm",
    "classifier": "StandardScaler fit on train only + LogisticRegression(C=0.5, class_weight=balanced, max_iter=1000)",
    "threshold_selection": "validation windows only: maximize F1, then precision, recall, higher threshold",
    "label_mapping": {label: int(label == "ambulance") for label in LABELS},
}


class DatasetError(RuntimeError):
    pass


@dataclass
class Session:
    metadata_path: Path
    raw_path: Path
    source_id: str
    label: str
    metadata: dict
    raw_sha256: str

    @property
    def target(self):
        return int(self.label == "ambulance")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def binary_name(label):
    return "ambulance" if label == "ambulance" else "non_target"


def canonical_source(value):
    if not isinstance(value, str) or not value.strip():
        raise DatasetError("Missing nonempty source_id; original-source identity is required for leakage-safe splitting")
    return value.strip().casefold()


def read_raw_channel(session):
    """Validate a complete recording, then return channel 0 at its TRUE rate."""
    with np.load(session.raw_path, allow_pickle=False) as saved:
        if not {"samples", "blkno", "rec_idx"}.issubset(saved.files):
            raise DatasetError("raw.npz must contain samples, blkno and rec_idx")
        samples, blockno, index = saved["samples"], saved["blkno"], saved["rec_idx"]
    if samples.shape != (BLOCKS, FRAMES, CHANNELS) or samples.dtype != np.dtype("int32"):
        raise DatasetError(f"Expected complete signed-int32 array {(BLOCKS, FRAMES, CHANNELS)}, got {samples.shape}/{samples.dtype}")
    if samples.min() < -(1 << 23) or samples.max() > (1 << 23) - 1:
        raise DatasetError("PCM24 values out of range")
    if index.shape != () or index.dtype.kind not in "iu" or int(index) != BLOCKS:
        raise DatasetError("rec_idx is not exactly 768; partial/wrapped captures are excluded")
    if blockno.shape != (BLOCKS,) or blockno.dtype.kind not in "iu" or blockno.min() < 0 or blockno.max() > 0xffffffff:
        raise DatasetError("Invalid saved block indices")
    increments = (blockno[1:].astype(np.uint64) - blockno[:-1].astype(np.uint64)) & np.uint64(0xffffffff)
    if np.any(increments != 1):
        raise DatasetError("Saved block-index gaps; stateful spectral/rhythm features must not concatenate missing blocks")
    channel = samples[:, :, 0].reshape(-1).astype(np.float64)
    channel -= channel.mean()
    if not np.any(channel):
        raise DatasetError("Channel 0 is silent or constant")
    return channel / float(1 << 23)


def inspect_dataset(dataset_root):
    """Read-only inventory. Rejected sessions remain reported, never auto-approved."""
    dataset_root = Path(dataset_root)
    paths = sorted(dataset_root.rglob("metadata.json")) if dataset_root.is_dir() else []
    entries, sessions, source_labels = [], [], defaultdict(set)
    observed_hashes = defaultdict(set)
    declared_roles = defaultdict(set)
    development_sources = set()
    fatal_errors = []
    for path in paths:
        entry = {"metadata_path": str(path), "accepted": False, "reasons": []}
        entries.append(entry)
        try:
            metadata = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(metadata, dict):
                raise DatasetError("Metadata root must be an object")
            if not isinstance(metadata.get("quality", {}), dict):
                raise DatasetError("quality must be an object")
            label = metadata.get("label")
            source_id = canonical_source(metadata.get("source_id"))
            entry.update(label=label, source_id=source_id, status=metadata.get("status"),
                         content_review_status=metadata.get("quality", {}).get("content_review_status", "not_reviewed"))
            if label not in LABELS:
                raise DatasetError(f"Unsupported label {label!r}")
            # Check even excluded takes: relabelling the same original source is ambiguous.
            source_labels[source_id].add(label)
            usage, role = metadata.get("source_usage"), metadata.get("split_role")
            entry.update(source_usage=usage, split_role=role)
            if usage == "development":
                development_sources.add(source_id)
                declared_roles[source_id].add("train")
            elif usage is not None:
                fatal_errors.append(f"Unsupported source_usage={usage!r} for {source_id!r}; supported declaration is 'development'.")
            if role is not None:
                if isinstance(role, str):
                    declared_roles[source_id].add(role)
                if role == "train":
                    development_sources.add(source_id)
                else:
                    fatal_errors.append(f"Unsupported reserved split_role={role!r} for {source_id!r}; only 'train' is supported. Do not force a development source into validation/test.")
            quality = metadata.get("quality", {})
            if metadata.get("status") != "complete":
                entry["reasons"].append("status_not_complete")
            if quality.get("content_review_status") != "approved":
                entry["reasons"].append("content_review_not_approved:" + str(quality.get("content_review_status", "not_reviewed")))
            if quality.get("validation_status") != "passed":
                entry["reasons"].append("structural_validation_not_passed")
            if entry["reasons"]:
                continue
            capture = metadata.get("capture", {})
            if not isinstance(capture, dict) or not isinstance(capture.get("channel_map", {}), dict):
                raise DatasetError("capture and channel_map must be objects")
            if capture.get("actual_sample_rate_hz") != SOURCE_FS or capture.get("resampled") is not False:
                raise DatasetError("Expected unresampled 48828.125 Hz source audio, not integer WAV-header rate")
            if capture.get("channel_map", {}).get("outer_ring") != [0, 1, 2, 3, 4, 5]:
                raise DatasetError("Outer-ring channel map must explicitly be [0,1,2,3,4,5]")
            counter_delta = quality.get("pipeline_counter_delta", {})
            if not isinstance(counter_delta, dict):
                raise DatasetError("pipeline_counter_delta must be an object")
            if any(value is not None and value != 0 for value in counter_delta.values()):
                raise DatasetError("Capture error counters increased; approval cannot override a failed capture")
            files = metadata.get("files", {})
            if not isinstance(files, dict) or not isinstance(files.get("raw", {}), dict):
                raise DatasetError("files and files.raw must be objects")
            raw_info = files.get("raw", {})
            relative = raw_info.get("file", "raw.npz")
            raw = (path.parent / relative).resolve()
            if raw.parent != path.parent.resolve() or not raw.is_file():
                raise DatasetError("Missing/session-external raw NPZ; WAV alone is insufficient for exact source-rate handling")
            digest = sha256(raw)
            if raw_info.get("sha256") and raw_info["sha256"] != digest:
                raise DatasetError("raw.npz SHA256 does not match metadata")
            session = Session(path, raw, source_id, label, metadata, digest)
            read_raw_channel(session)  # dry-run validates actual arrays too, without feature extraction.
            sessions.append(session)
            observed_hashes[digest].add(source_id)
            entry.update(accepted=True, binary_class=binary_name(label), raw_sha256=digest)
        except (OSError, ValueError, TypeError, KeyError, DatasetError) as exc:
            entry["reasons"].append(str(exc))
    for source_id, labels in sorted(source_labels.items()):
        if len(labels) > 1:
            fatal_errors.append(f"Conflicting labels for source_id={source_id!r}: {sorted(labels)}. Fix provenance manually; no split/model is allowed.")
    for source_id, roles in sorted(declared_roles.items()):
        if len(roles) > 1:
            fatal_errors.append(f"Conflicting reserved split roles for source_id={source_id!r}: {sorted(roles)}. No model is allowed.")
    for digest, source_ids in observed_hashes.items():
        if len(source_ids) > 1:
            fatal_errors.append(f"Identical raw NPZ {digest} appears under different source_id groups {sorted(source_ids)}; possible source leakage.")
    groups = {name: sorted({session.source_id for session in sessions if binary_name(session.label) == name}) for name in ("ambulance", "non_target")}
    missing = {name: max(0, MIN_SOURCES_PER_CLASS - len(values)) for name, values in groups.items()}
    non_development = {name: [source for source in values if source not in development_sources] for name, values in groups.items()}
    missing_non_development = {name: max(0, MIN_NON_DEVELOPMENT_SOURCES_PER_CLASS - len(values)) for name, values in non_development.items()}
    reason_counts = Counter(reason for entry in entries for reason in entry["reasons"])
    review_counts = Counter(entry.get("content_review_status", "invalid_metadata") for entry in entries)
    status_counts = Counter(entry.get("status", "invalid_metadata") for entry in entries)
    inventory = {
        "tool_version": VERSION, "dataset_root": str(dataset_root), "metadata_files": len(paths),
        "accepted_sessions": len(sessions), "excluded_sessions": len(entries) - len(sessions),
        "approved_session_label_counts": dict(Counter(session.label for session in sessions)),
        "hard_negative_labels_without_approved_sessions": [label for label in ("police", "fire", "horn", "speech", "music") if not any(session.label == label for session in sessions)],
        "status_counts": dict(status_counts), "content_review_status_counts": dict(review_counts),
        "exclusion_reason_counts": dict(reason_counts), "sessions": entries,
        "independent_source_groups": groups,
        "minimum_independent_sources_per_binary_class": MIN_SOURCES_PER_CLASS,
        "split_policy_version": SPLIT_POLICY_VERSION,
        "development_source_groups": sorted(development_sources),
        "non_development_source_groups": non_development,
        "minimum_non_development_sources_per_binary_class": MIN_NON_DEVELOPMENT_SOURCES_PER_CLASS,
        "missing_non_development_sources": missing_non_development,
        "missing_independent_sources": missing, "fatal_errors": fatal_errors,
        "ready_for_training": not fatal_errors and all(value == 0 for value in missing.values()) and all(value == 0 for value in missing_non_development.values()),
        "human_labels_automatically_verified": False,
        "notes": [
            "Approval is a human listening/label decision. This tool never changes metadata or automatically approves sound identity.",
            "Every replay, segment, re-recording and augmentation of an original clip must retain the same source_id; different IDs cannot prove independent origins.",
            "A source_usage='development' or split_role='train' declaration in ANY same-source session locks that entire group to training, even when that particular session is excluded. Prior debugging/tuning exposure must be declared manually.",
            "Complete and block-index-contiguous captures do not independently prove zero lost audio samples; see capture metadata/counters.",
        ],
    }
    return sessions, inventory


def require_ready(inventory):
    errors = list(inventory["fatal_errors"])
    if not inventory["ready_for_training"]:
        for name, missing in inventory["missing_independent_sources"].items():
            if missing:
                have = len(inventory["independent_source_groups"][name])
                errors.append(f"{name}: only {have} approved independent source(s); need {MIN_SOURCES_PER_CLASS}, missing {missing}.")
        for name, missing in inventory.get("missing_non_development_sources", {}).items():
            if missing:
                have = len(inventory["non_development_source_groups"][name])
                errors.append(f"{name}: only {have} approved non-development source(s); need at least 2 for independent validation/test, missing {missing}.")
        raise DatasetError("Training refused; no model written. " + " ".join(errors))


def split_sources(sessions, seed=7020, development_sources=None):
    """Deterministic hash ranking, stratified by binary class; never split a source.

    Assignment is reproducible for an unchanged source inventory and seed. Adding
    sources can change ranks: each trained report records its exact split manifest.
    Do not compare runs as the same held-out experiment after changing the manifest.
    """
    development_sources = set(development_sources or ())
    development_sources.update(session.source_id for session in sessions if session.metadata.get("source_usage") == "development" or session.metadata.get("split_role") == "train")
    mapping = {}
    for target in (0, 1):
        sources = {session.source_id for session in sessions if session.target == target}
        if len(sources) < MIN_SOURCES_PER_CLASS:
            raise DatasetError(f"Binary class {target} requires at least three independent sources")
        development = sources & development_sources
        independent = sources - development
        if len(independent) < MIN_NON_DEVELOPMENT_SOURCES_PER_CLASS:
            raise DatasetError(f"Binary class {target} requires at least two non-development sources for validation/test; development sources are train-only")
        ranked = sorted(independent, key=lambda source: hashlib.sha256(f"{VERSION}:{seed}:{target}:{source}".encode("utf-8")).hexdigest())
        if development:
            mapping.update({source: "train" for source in development})
            # Once development/training sources exist, preserve all remaining
            # unseen sources for validation/test instead of consuming them in train.
            for index, source in enumerate(ranked):
                mapping[source] = "test" if index % 2 == 0 else "validation"
            continue
        # Three sources -> one each; larger datasets approach 60/20/20.
        heldout = max(1, int(len(ranked) * 0.2))
        for index, source in enumerate(ranked):
            mapping[source] = "test" if index < heldout else "validation" if index < 2 * heldout else "train"
    return mapping


def mel_bank():
    bins = np.fft.rfftfreq(CONFIG["fft_size"], 1.0 / TARGET_FS)
    to_mel = lambda hz: 2595.0 * np.log10(1.0 + hz / 700.0)
    to_hz = lambda mel: 700.0 * (10.0 ** (mel / 2595.0) - 1.0)
    edges = to_hz(np.linspace(to_mel(CONFIG["mel_min_hz"]), to_mel(CONFIG["mel_max_hz"]), CONFIG["mel_bins"] + 2))
    return np.array([np.maximum(0.0, np.minimum((bins - left) / (center - left), (right - bins) / (right - center)))
                     for left, center, right in zip(edges[:-2], edges[1:-1], edges[2:])])


MEL_BANK = mel_bank()
SCALAR_NAMES = ["ridge_q10", "ridge_q50", "ridge_q90", "ridge_std", "ridge_q90_minus_q10",
                "ridge_abs_step_mean", "ridge_reversal_fraction", "concentration_mean", "concentration_std",
                "tonal_fraction", "flatness_mean", "flatness_std", "ridge_modulation_hz_over_8",
                "ridge_modulation_peak_fraction", "ridge_autocorrelation_peak", "envelope_log_energy_std",
                "envelope_abs_step_mean", "envelope_modulation_hz_over_8", "envelope_modulation_peak_fraction"]
FEATURE_NAMES = [f"mel_{index:02d}_{stat}" for stat in ("mean", "std", "abs_delta_mean") for index in range(32)] + SCALAR_NAMES


def modulation(values):
    """Dominant 0.5..8 Hz fluctuation and its normalized periodic strength."""
    centered = values - np.mean(values)
    if np.std(centered) < 1e-10:
        return 0.0, 0.0
    spectrum = np.abs(np.fft.rfft(centered * np.hanning(len(centered)))) ** 2
    frequency = np.fft.rfftfreq(len(centered), CONFIG["frame_hop_seconds"])
    band = (frequency >= 0.5) & (frequency <= 8)
    energy = spectrum[band]
    if not np.any(energy):
        return 0.0, 0.0
    maximum = int(np.argmax(energy))
    return float(frequency[band][maximum] / 8.0), float(energy[maximum] / max(np.sum(energy), 1e-15))


def extract_window(audio):
    frame_size = int(TARGET_FS * CONFIG["frame_seconds"])
    hop = int(TARGET_FS * CONFIG["frame_hop_seconds"])
    frames = np.lib.stride_tricks.sliding_window_view(audio, frame_size)[::hop]
    hann = signal.windows.hann(frame_size, sym=False)
    spectrum = np.fft.rfft(frames * hann, n=CONFIG["fft_size"], axis=1)
    power = np.abs(spectrum) ** 2
    mel = power @ MEL_BANK.T
    log_mel = np.log(np.maximum(mel / np.maximum(mel.sum(axis=1, keepdims=True), 1e-15), 1e-12))
    bins = np.fft.rfftfreq(CONFIG["fft_size"], 1.0 / TARGET_FS)
    band = (bins >= CONFIG["mel_min_hz"]) & (bins <= CONFIG["mel_max_hz"])
    band_power = power[:, band]
    total = np.maximum(band_power.sum(axis=1), 1e-15)
    ridge = bins[band][np.argmax(band_power, axis=1)] / CONFIG["mel_max_hz"]
    concentration = band_power.max(axis=1) / total
    flatness = np.exp(np.mean(np.log(np.maximum(band_power, 1e-15)), axis=1)) / np.maximum(np.mean(band_power, axis=1), 1e-15)
    ridge_step = np.diff(ridge)
    # Ignore one-bin jitter when counting rise/fall reversals.
    moving = ridge_step[np.abs(ridge_step) >= 50.0 / CONFIG["mel_max_hz"]]
    reversals = np.mean(moving[1:] * moving[:-1] < 0) if len(moving) > 1 else 0.0
    ridge_mod = modulation(ridge)
    centered = ridge - ridge.mean()
    correlation = signal.correlate(centered, centered, mode="full", method="direct")[len(centered) - 1:]
    denominator = max(float(correlation[0]), 1e-15)
    autocorrelation = float(np.max(correlation[10:101]) / denominator)
    envelope = np.log(np.maximum(total, 1e-15))
    envelope_mod = modulation(envelope)
    q10, q50, q90 = np.percentile(ridge, [10, 50, 90])
    scalar = [q10, q50, q90, ridge.std(), q90 - q10,
              np.mean(np.abs(ridge_step)), reversals, concentration.mean(), concentration.std(),
              np.mean(concentration >= 0.15), flatness.mean(), flatness.std(),
              *ridge_mod, autocorrelation, envelope.std(), np.mean(np.abs(np.diff(envelope))), *envelope_mod]
    result = np.r_[log_mel.mean(axis=0), log_mel.std(axis=0), np.abs(np.diff(log_mel, axis=0)).mean(axis=0), scalar]
    if result.shape != (len(FEATURE_NAMES),) or not np.isfinite(result).all():
        raise DatasetError("Nonfinite or unexpected feature vector")
    return result


def session_windows(session):
    original = read_raw_channel(session)
    audio = signal.resample_poly(original, CONFIG["resample_up"], CONFIG["resample_down"], window=("kaiser", 5.0))
    window = int(TARGET_FS * CONFIG["window_seconds"])
    hop = int(TARGET_FS * CONFIG["window_hop_seconds"])
    for start in range(0, len(audio) - window + 1, hop):
        yield start / TARGET_FS, extract_window(audio[start:start + window])


def feature_dataset(sessions, split_map):
    features, labels, rows = [], [], []
    for session in sessions:
        for start, vector in session_windows(session):
            features.append(vector)
            labels.append(session.target)
            rows.append({"source_id": session.source_id, "label": session.label, "target": session.target,
                         "metadata_path": str(session.metadata_path), "start_seconds": start,
                         "end_seconds": start + CONFIG["window_seconds"], "split": split_map[session.source_id]})
    if not features:
        raise DatasetError("No complete 2-second windows")
    return np.asarray(features), np.asarray(labels, dtype=np.int8), rows


def metrics(y_true, probability, threshold):
    predicted = (probability >= threshold).astype(np.int8)
    precision, recall, f1, _ = precision_recall_fscore_support(y_true, predicted, average="binary", zero_division=0)
    confusion = confusion_matrix(y_true, predicted, labels=[0, 1])
    tn, fp, fn, tp = map(int, confusion.ravel())
    return {"windows": len(y_true), "precision": float(precision), "recall": float(recall), "f1": float(f1),
            "confusion_matrix_order": ["non_target", "ambulance"], "confusion_matrix": confusion.tolist(),
            "true_negative_windows": tn, "false_positive_windows": fp, "false_negative_windows": fn,
            "true_positive_windows": tp, "false_positive_rate": fp / (tn + fp) if tn + fp else None}


def select_threshold(y_validation, validation_probability):
    if set(map(int, y_validation)) != {0, 1}:
        raise DatasetError("Validation set must contain both classes")
    candidates = np.unique(np.r_[0.0, validation_probability, np.nextafter(1.0, 2.0)])
    scored = []
    for threshold in candidates:
        score = metrics(y_validation, validation_probability, float(threshold))
        scored.append(((score["f1"], score["precision"], score["recall"], float(threshold)), float(threshold)))
    return max(scored)[1]


def train(sessions, inventory, *, seed=7020, output_dir=None):
    require_ready(inventory)  # must precede directory creation or model writing.
    development_sources = set(inventory.get("development_source_groups", ()))
    split_map = split_sources(sessions, seed, development_sources)
    split_details = {source: {"split": split, "source_usage": "development" if source in development_sources else "not_development_declared"} for source, split in split_map.items()}
    features, labels, rows = feature_dataset(sessions, split_map)
    indices = {split: np.array([index for index, row in enumerate(rows) if row["split"] == split], dtype=int) for split in SPLITS}
    for split, selected in indices.items():
        if set(map(int, labels[selected])) != {0, 1}:
            raise DatasetError(f"{split} does not contain both classes after feature extraction")
    model = make_pipeline(StandardScaler(), LogisticRegression(C=0.5, class_weight="balanced", max_iter=1000, random_state=seed))
    model.fit(features[indices["train"]], labels[indices["train"]])
    # Neither fitting nor threshold selection ever sees test-set labels/predictions.
    validation_probability = model.predict_proba(features[indices["validation"]])[:, 1]
    threshold = select_threshold(labels[indices["validation"]], validation_probability)
    split_reports = {}
    for split, selected in indices.items():
        probability = validation_probability if split == "validation" else model.predict_proba(features[selected])[:, 1]
        score = metrics(labels[selected], probability, threshold)
        selected_rows = [rows[index] for index in selected]
        score["source_groups"] = sorted({row["source_id"] for row in selected_rows})
        score["false_positive_examples"] = [dict(row, ambulance_score=float(p)) for row, p in zip(selected_rows, probability) if not row["target"] and p >= threshold]
        score["false_negative_examples"] = [dict(row, ambulance_score=float(p)) for row, p in zip(selected_rows, probability) if row["target"] and p < threshold]
        score["by_original_label"] = {label: {"windows": sum(row["label"] == label for row in selected_rows),
                                            "predicted_ambulance_windows": int(sum(row["label"] == label and p >= threshold for row, p in zip(selected_rows, probability)))}
                                      for label in sorted({row["label"] for row in selected_rows})}
        split_reports[split] = score
    report = {
        "tool_version": VERSION, "created_at_utc": datetime.now(timezone.utc).isoformat(), "seed": seed,
        "feature_config": CONFIG, "feature_names": FEATURE_NAMES, "feature_count": len(FEATURE_NAMES),
        "inventory": inventory, "source_split_manifest": split_map,
        "source_split_details": split_details, "split_policy_version": SPLIT_POLICY_VERSION,
        "development_source_groups": sorted(development_sources),
        "split_method": "Each source used for development is train-only (any same-source session declaration applies). Within each binary class, fixed-seed SHA256 ranks non-development sources. If development sources exist, alternate remaining sources between test/validation; otherwise retain 60/20/20-style train/validation/test ranks. At least two non-development sources per class. Windows never cross source groups.",
        "split_inventory_change_note": "Adding/removing source groups may change hash ranks. This report fixes the exact manifest for this run; preserve it when comparing experiments.",
        "threshold": threshold, "threshold_selected_on": "validation", "test_used_for_threshold": False,
        "evaluation_unit": "overlapping 2-second windows, 1-second hop; windows from one source are correlated",
        "splits": split_reports,
        "detection_latency": {"measured": False, "reason": "No annotated event onset/offset or real-time run; 2-second feature context is not a measured detection delay."},
        "labels": {"provenance": "operator listening approval", "machine_verified": False},
        "deployment": {"target": "PC baseline only", "arm_or_fpga_model_deployed": False},
        "limitations": [
            "Three independent sources per class is only a runnable minimum, not a sufficient competition/generalization dataset.",
            "Ambulance/police/fire sirens may overlap acoustically. Independent labelled hard-negative sources and real recordings are required.",
            "No multi-source association, direction estimation or localization metric is measured by this classifier.",
            "No real-world accuracy or latency is implied by running synthetic tests or fitting a model.",
            "Logistic-regression scores have not been probability-calibrated.",
        ],
        "library_versions": {"numpy": np.__version__, "scipy": scipy.__version__, "scikit_learn": sklearn.__version__, "joblib": joblib.__version__},
    }
    if output_dir is None:
        suffix = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "_" + uuid.uuid4().hex[:8]
        output_dir = ROOT / "build" / ("ambulance_baseline_" + suffix)
    output_dir = Path(output_dir)
    if output_dir.exists():
        raise DatasetError("Output directory already exists; choose a new directory to avoid overwriting a model")
    output_dir.mkdir(parents=True, exist_ok=False)
    bundle = {"tool_version": VERSION, "estimator": model, "threshold": threshold,
              "feature_config": CONFIG, "feature_names": FEATURE_NAMES, "source_split_manifest": split_map,
              "source_split_details": split_details, "split_policy_version": SPLIT_POLICY_VERSION,
              "library_versions": report["library_versions"]}
    joblib.dump(bundle, output_dir / "model.joblib")
    report["model_file"] = "model.joblib"
    report["model_sha256"] = sha256(output_dir / "model.joblib")
    (output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output_dir / "feature_config.json").write_text(json.dumps(dict(CONFIG, feature_names=FEATURE_NAMES), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return output_dir, report


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset-root", type=Path, default=ROOT / "rec/dataset")
    parser.add_argument("--dry-run", "--validate-only", dest="dry_run", action="store_true", help="Validate/review/source-count inventory only; never write model/output directory")
    parser.add_argument("--output-dir", type=Path, help="New directory for model.joblib/report.json/feature_config.json")
    parser.add_argument("--seed", type=int, default=7020)
    return parser


def main(argv=None):
    args = make_parser().parse_args(argv)
    sessions, inventory = inspect_dataset(args.dataset_root)
    print(json.dumps(inventory, ensure_ascii=False, indent=2))
    try:
        require_ready(inventory)
        if args.dry_run:
            print("DRY RUN: eligible complete/approved sessions passed validation; no feature fitting or model written.")
            return 0
        output, report = train(sessions, inventory, seed=args.seed, output_dir=args.output_dir)
        print(f"PC baseline written to: {output}")
        print("Test window metrics: " + json.dumps(report["splits"]["test"], ensure_ascii=False))
        print("Detection latency was NOT measured; human labels are NOT automatically verified.")
        return 0
    except (DatasetError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
