"""Collect one user-labelled, continuous 16.1 s array recording via rec_dump.py.

Run from any directory (the board must already run the matching sound_loc.elf):
  python -I tools/capture_labelled.py --label ambulance --source-id ambulance_clip_A \
      --distance-m 1 --angle-deg 0 --volume "speaker 50%" --room lab

The label is supplied by the operator, NOT recognized or independently verified.
Press Enter only once the source is playing steadily. Recording then starts
immediately, without a sound trigger. This script does not program the board.
Only structurally complete sessions without observed pipeline errors are marked
complete. A completed session still needs listening/label review before training.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid
import wave

import numpy as np


ROOT = Path(__file__).resolve().parent.parent
LABELS = ("ambulance", "police", "fire", "horn", "speech", "music", "clap", "background")
COUNTERS = ("capture_seq_errors", "capture_short_packets", "capture_dma_errors")
FS_HZ = 48828.125
WAV_FS_HZ = 48828  # RIFF/WAVE stores sample rate as an unsigned INTEGER.
BLOCKS = 768
FRAMES = 1024
CHANNELS = 8
OUTER_CHANNELS = (0, 1, 2, 3, 4, 5)
DURATION_SECONDS = BLOCKS * FRAMES / FS_HZ


class CaptureError(RuntimeError):
    pass


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def write_json(path, value):
    """Replace only our own metadata file; session mkdir itself never overwrites."""
    path = Path(path)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_status(output):
    match = re.search(r"mode=(\d+)\s+recorded_blocks=(\d+)\s+left=(-?\d+)\s+frozen=(\d+)", output)
    if not match:
        raise CaptureError("rec_dump --status did not return a valid status line; check JTAG, board power and running ELF. Output: " + output[-2000:])
    return dict(zip(("mode", "recorded_blocks", "left", "frozen"), map(int, match.groups())))


def validate_recording(path):
    """Validate saved block indices; this alone cannot prove DMA sample continuity."""
    with np.load(path, allow_pickle=False) as recording:
        missing = {"samples", "blkno", "rec_idx"} - set(recording.files)
        if missing:
            raise CaptureError("NPZ missing fields: " + ", ".join(sorted(missing)))
        samples = recording["samples"]
        block_numbers = recording["blkno"]
        rec_idx = recording["rec_idx"]
    if samples.shape != (BLOCKS, FRAMES, CHANNELS):
        raise CaptureError(f"Incomplete/wrong samples shape {samples.shape}; expected {(BLOCKS, FRAMES, CHANNELS)}")
    if samples.dtype != np.dtype("int32"):
        raise CaptureError(f"samples must be signed int32 containing PCM24, got {samples.dtype}")
    if samples.min() < -(1 << 23) or samples.max() > (1 << 23) - 1:
        raise CaptureError("samples contain values outside signed PCM24 range")
    if block_numbers.shape != (BLOCKS,) or block_numbers.dtype.kind not in "iu":
        raise CaptureError("blkno must contain exactly 768 integer block indices")
    if block_numbers.min() < 0 or block_numbers.max() > 0xffffffff:
        raise CaptureError("blkno must contain unsigned 32-bit block-index values")
    if rec_idx.shape != () or rec_idx.dtype.kind not in "iu" or int(rec_idx) != BLOCKS:
        raise CaptureError("rec_idx must be exactly 768: wrapped/partial recordings are rejected")
    # uint32 modulo also supports a legitimate counter wrap at 2**32.
    delta = (block_numbers[1:].astype(np.uint64) - block_numbers[:-1].astype(np.uint64)) & np.uint64(0xffffffff)
    gap_count = int(np.count_nonzero(delta != 1))
    if gap_count:
        raise CaptureError(f"Saved block indices are not continuous ({gap_count} gaps); re-record this session")
    summary = {
        "shape": list(samples.shape), "dtype": str(samples.dtype),
        "recorded_blocks": BLOCKS, "rec_idx": int(rec_idx),
        "first_block_index": int(block_numbers[0]), "last_block_index": int(block_numbers[-1]),
        "block_index_gap_count": 0, "block_indices_contiguous": True,
        "dma_word_headers_preserved_in_npz": False,
        "sample_continuity_proven_from_npz": False,
        "continuity_limit": "Consecutive blkno only proves consecutive saved processing-block indices; NPZ omits DMA word headers and cannot independently prove no lost audio samples.",
        "actual_sample_rate_hz": FS_HZ, "duration_seconds": DURATION_SECONDS,
        "per_channel_peak_abs": np.abs(samples.astype(np.int64)).max(axis=(0, 1)).tolist(),
        "outer_channels_all_zero": bool(not np.any(samples[:, :, :6])),
    }
    if summary["outer_channels_all_zero"]:
        raise CaptureError("All six outer microphone channels are zero; check I2S capture/wiring")
    return samples, summary


def export_outer_wav(path, samples):
    """Write interleaved six-channel PCM24 WITHOUT resampling or normalization."""
    pcm = np.ascontiguousarray(samples[:, :, OUTER_CHANNELS]).reshape(-1, 6)
    unsigned = pcm.astype(np.uint32) & np.uint32(0xffffff)
    packed = np.empty((*unsigned.shape, 3), dtype=np.uint8)
    packed[:, :, 0] = unsigned & 255
    packed[:, :, 1] = (unsigned >> 8) & 255
    packed[:, :, 2] = (unsigned >> 16) & 255
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(6)
        stream.setsampwidth(3)
        stream.setframerate(WAV_FS_HZ)
        stream.writeframes(packed.tobytes())
    return {
        "file": Path(path).name, "encoding": "PCM_S24LE", "channels": 6,
        "channel_order_original_indices": list(OUTER_CHANNELS),
        "frames": BLOCKS * FRAMES, "header_sample_rate_hz": WAV_FS_HZ,
        "actual_sample_rate_hz": FS_HZ, "resampled": False, "normalized": False,
        "sample_rate_note": "Standard WAV sample-rate field is integer: 48828 Hz is the nearest integer to actual 48828.125 Hz. No resampling occurred; use metadata actual rate for timing/localization.",
    }


# Run the existing helper in a separate process so JTAG calls have a hard timeout.
# Missing symbols are reported as unavailable; an actual debugger failure fails the session.
COUNTER_HELPER = r'''
import json, pathlib, re, sys
sys.path.insert(0, str(pathlib.Path(sys.argv[1]).parent))
import rec_dump
names = ("capture_seq_errors", "capture_short_packets", "capture_dma_errors")
syms = rec_dump.symbols()
present = [n for n in names if n in syms]
result = {n: None for n in names}
if present:
    tcl = "".join('puts "CAPTURE_ONE %s [mrd -value %s 1]"\n' % (n, rec_dump.addr(syms, n)) for n in present)
    out = rec_dump.run_tcl(tcl)
    for name in present:
        match = re.search(r"^CAPTURE_ONE " + name + r"\s+(\S+)", out, re.M)
        if not match:
            raise RuntimeError("Failed reading pipeline counter " + name + ": " + out[-2000:])
        raw = match.group(1)
        result[name] = int(raw, 16) if raw.lower().startswith("0x") else int(raw)
print("CAPTURE_COUNTERS " + json.dumps(result, sort_keys=True))
'''


class RecDumpBackend:
    def __init__(self, session, command_timeout=90.0):
        self.session = Path(session)
        self.command_timeout = command_timeout
        self.tool = Path(__file__).resolve().with_name("rec_dump.py")

    def _run(self, command, *, timeout=None):
        event = {"at_utc": utc_now(), "command": list(map(str, command))}
        timeout = self.command_timeout if timeout is None else min(self.command_timeout, timeout)
        try:
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout, check=False)
            event.update(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
            if result.returncode:
                raise CaptureError(f"Command failed (exit {result.returncode}): {' '.join(map(str, command))}\n{result.stderr[-3000:]}\n{result.stdout[-2000:]}")
            return result.stdout
        except subprocess.TimeoutExpired as exc:
            event.update(error=f"Timed out after {timeout}s", stdout=str(exc.stdout or ""), stderr=str(exc.stderr or ""))
            raise CaptureError(f"JTAG command timed out after {timeout}s; check cable/power and close competing debugger sessions") from exc
        finally:
            with (self.session / "commands.jsonl").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(event, ensure_ascii=False) + "\n")

    def command(self, *args, timeout=None):
        return self._run([sys.executable, "-I", str(self.tool), *map(str, args)], timeout=timeout)

    def counters(self):
        output = self._run([sys.executable, "-I", "-c", COUNTER_HELPER, str(self.tool)])
        lines = [line[len("CAPTURE_COUNTERS "):] for line in output.splitlines() if line.startswith("CAPTURE_COUNTERS ")]
        if len(lines) != 1:
            raise CaptureError("Counter reader returned no unique result; check JTAG and matching ELF")
        result = json.loads(lines[0])
        if set(result) != set(COUNTERS) or any(v is not None and (not isinstance(v, int) or not 0 <= v <= 0xffffffff) for v in result.values()):
            raise CaptureError("Invalid pipeline counter result")
        return result


def create_session(dataset_root, label, source_id):
    source_slug = re.sub(r"[^a-zA-Z0-9._-]+", "_", source_id).strip("._-")[:60] or "source"
    parent = Path(dataset_root) / label
    parent.mkdir(parents=True, exist_ok=True)
    for _ in range(5):
        session = parent / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ") + "_" + source_slug + "_" + uuid.uuid4().hex[:8])
        try:
            session.mkdir(exist_ok=False)
            return session
        except FileExistsError:
            continue
    raise CaptureError("Could not allocate a unique session directory")


def capture(args, *, backend_factory=RecDumpBackend, prompt=input, clock=time.monotonic, sleep=time.sleep):
    session = create_session(args.dataset_root, args.label, args.source_id)
    metadata_path = session / "metadata.json"
    metadata = {
        "schema_version": 1, "session_id": session.name, "created_at_utc": utc_now(),
        "status": "waiting_for_operator", "label": args.label, "source_id": args.source_id,
        "label_provenance": "user_provided_unverified", "label_machine_verified": False,
        "source_description": args.source_description,
        "operator_metadata": {"distance_m": args.distance_m, "angle_deg": args.angle_deg, "volume": args.volume, "room": args.room, "notes": args.notes},
        "capture": {"mode": "continuous_no_sound_trigger", "expected_blocks": BLOCKS,
                    "frames_per_block": FRAMES, "actual_sample_rate_hz": FS_HZ,
                    "expected_duration_seconds": DURATION_SECONDS, "raw_channels": CHANNELS,
                    "sample_encoding": "signed_PCM24_in_int32", "resampled": False,
                    "channel_map": {"outer_ring": [0, 1, 2, 3, 4, 5], "unused_zero": [6], "center": 7,
                                    "angle_origin": "ch0", "positive_direction": "counterclockwise"}},
        "firmware": {"local_elf": str(ROOT / "sw/workspace/sound_loc/Debug/sound_loc.elf"),
                     "running_target_matches_local_elf": "not_independently_verified"},
        "quality": {"validation_status": "not_run", "label_review_required": True,
                    "counter_window_note": "Counter snapshots bracket recording start through frozen-state confirmation, including short orchestration intervals; conservative error rejection."},
        "files": {}, "status_history": [],
    }
    write_json(metadata_path, metadata)
    print(f"Session: {session}")
    print(f"Label={args.label}; source-id={args.source_id} (operator supplied, not verified)")
    if args.label == 'background':
        print("Pause sound playback. Keep the room quiet (no speech/tapping) for at least 17 seconds.")
    else:
        print("Start a single labelled sound source. Keep level/position fixed for at least 17 seconds.")
    print("The board must run the ELF matching rec_dump.py symbols; this tool does not flash firmware.")
    try:
        prompt("Press Enter when the room is quiet; continuous capture starts next: " if args.label == 'background'
               else "Press Enter when the source is already playing steadily; continuous capture starts next: ")
        backend = backend_factory(session, args.command_timeout)
        local_elf = Path(metadata["firmware"]["local_elf"])
        if local_elf.is_file():
            metadata["firmware"]["local_elf_sha256"] = sha256(local_elf)
        metadata["counter_before"] = backend.counters()
        metadata["status"] = "recording"
        metadata["recording_requested_at_utc"] = utc_now()
        write_json(metadata_path, metadata)
        started = clock()
        backend.command("--continuous", timeout=args.timeout_seconds)
        while True:
            remaining = args.timeout_seconds - (clock() - started)
            if remaining <= 0:
                raise CaptureError(f"Capture did not freeze within {args.timeout_seconds}s; no valid sample accepted")
            status = parse_status(backend.command("--status", timeout=remaining))
            metadata["status_history"].append({"at_utc": utc_now(), **status})
            write_json(metadata_path, metadata)
            print(f"blocks={status['recorded_blocks']}/{BLOCKS} left={status['left']} frozen={status['frozen']}")
            if status["frozen"] == 1:
                if status["mode"] != 1 or status["recorded_blocks"] != BLOCKS or status["left"] != 0:
                    raise CaptureError(f"Frozen recording is partial or wrong mode: {status}")
                break
            sleep(min(args.poll_seconds, max(0, args.timeout_seconds - (clock() - started))))
        metadata["counter_after"] = backend.counters()
        counter_delta = {}
        for name in COUNTERS:
            before, after = metadata["counter_before"][name], metadata["counter_after"][name]
            if (before is None) != (after is None):
                raise CaptureError(f"Pipeline counter availability changed: {name}")
            counter_delta[name] = None if before is None else (after - before) & 0xffffffff
        metadata["quality"]["pipeline_counter_delta"] = counter_delta
        metadata["quality"]["pipeline_counters_available"] = all(v is not None for v in counter_delta.values())
        metadata["status"] = "reading"
        write_json(metadata_path, metadata)
        raw_path = session / "raw.npz"
        backend.command(raw_path)
        if not raw_path.is_file():
            raise CaptureError("rec_dump returned without writing raw.npz")
        metadata["files"]["raw"] = {"file": raw_path.name, "sha256": sha256(raw_path)}
        samples, summary = validate_recording(raw_path)
        metadata["quality"].update(summary)
        if any(v for v in counter_delta.values() if v is not None):
            raise CaptureError(f"Pipeline errors increased during capture window: {counter_delta}; retained raw file is diagnostic only, re-record")
        if not args.no_wav:
            wav_path = session / "outer6_pcm24.wav"
            wav_info = export_outer_wav(wav_path, samples)
            wav_info["sha256"] = sha256(wav_path)
            metadata["files"]["wav"] = wav_info
        metadata["status"] = "complete"
        metadata["completed_at_utc"] = utc_now()
        metadata["quality"]["validation_status"] = "passed"
        metadata["quality"]["pipeline_counters_note"] = "No increments observed in available counters; this is not an independent end-to-end proof of zero lost samples." if any(v is not None for v in counter_delta.values()) else "Counters absent in this ELF: DMA/sample continuity beyond saved block indices was not checked."
        write_json(metadata_path, metadata)
        print(f"Complete: {DURATION_SECONDS:.6f} s, six-channel export at original rate (no resampling).")
        print("Listen and verify the label; complete does not certify ambulance identity or detector accuracy.")
        return session, True
    except (Exception, KeyboardInterrupt) as exc:
        metadata["status"] = "cancelled" if isinstance(exc, KeyboardInterrupt) else "failed"
        metadata["quality"]["validation_status"] = "failed"
        metadata["error"] = {"type": type(exc).__name__, "message": str(exc)}
        metadata["failed_at_utc"] = utc_now()
        write_json(metadata_path, metadata)
        print(f"FAILED (not a valid training sample): {exc}\nDetails: {metadata_path}", file=sys.stderr)
        return session, False


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--label", required=True, choices=LABELS)
    parser.add_argument("--source-id", required=True, help="Stable ID of original clip/source; keep all recordings from one source in one dataset split")
    parser.add_argument("--source-description", default="", help="Original URL/name/license or physical source description")
    parser.add_argument("--distance-m", type=float, help="Operator measured speaker-array distance; not an algorithm estimate")
    parser.add_argument("--angle-deg", type=float, help="Operator measured direction: ch0=0, counterclockwise positive")
    parser.add_argument("--volume", default=None, help="Speaker/player setting, for example speaker 50 percent")
    parser.add_argument("--room", default=None)
    parser.add_argument("--notes", default="")
    parser.add_argument("--dataset-root", type=Path, default=ROOT / "rec/dataset")
    parser.add_argument("--timeout-seconds", type=float, default=90, help="Maximum wait for recording to freeze (default 90 s, excludes dump)")
    parser.add_argument("--command-timeout", type=float, default=120, help="Hard subprocess timeout for each JTAG command/dump (default 120 s)")
    parser.add_argument("--poll-seconds", type=float, default=2)
    parser.add_argument("--no-wav", action="store_true", help="Keep NPZ+metadata only")
    return parser


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    args.source_id = args.source_id.strip()
    if not args.source_id:
        parser.error("--source-id must identify an original source, not an empty string")
    for name in ("timeout_seconds", "command_timeout", "poll_seconds"):
        if not np.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            parser.error(name.replace("_", "-") + " must be finite and positive")
    if args.timeout_seconds < DURATION_SECONDS + 5:
        parser.error("--timeout-seconds must allow at least 22 seconds for the 16.1 second buffer")
    if args.distance_m is not None and (not np.isfinite(args.distance_m) or args.distance_m < 0):
        parser.error("--distance-m must be finite and nonnegative")
    if args.angle_deg is not None and (not np.isfinite(args.angle_deg) or not 0 <= args.angle_deg < 360):
        parser.error("--angle-deg must be in [0, 360)")
    _, ok = capture(args)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
