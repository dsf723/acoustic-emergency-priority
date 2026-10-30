"""Offline diagnostic of a user-labelled reference clip against the current rule.

This does not play audio or access the board. Array channels are synthesized from
one mono recording with ideal far-field delays, not recorded microphone channels.
The source label is user-supplied; no ambulance/police/fire identity is verified.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build" / "python_deps"))

import miniaudio
import numpy as np
from scipy.signal import resample_poly


def intervals(mask, block_seconds, offset):
    result = []
    start = None
    for i, value in enumerate(list(mask) + [False]):
        if value and start is None:
            start = i
        if not value and start is not None:
            result.append([round(start * block_seconds - offset, 3),
                           round(i * block_seconds - offset, 3)])
            start = None
    return result


def percentiles(values):
    a = np.asarray(values, dtype=float)
    return {str(p): round(float(np.percentile(a, p)), 4)
            for p in [0, 10, 50, 90, 100]} if len(a) else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--label", default="user-labelled ambulance siren reference")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build" / "reference_188809_analysis.json")
    parser.add_argument("--azimuth", type=float, default=0.0)
    args = parser.parse_args()
    source = args.source.resolve()
    spec = importlib.util.spec_from_file_location("reference_gen_test", ROOT / "sw/test/gen_test.py")
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    info = miniaudio.get_file_info(str(source))
    # Decode bytes because this miniaudio Windows build cannot decode the
    # Chinese filename through decode_file(), although metadata reads succeed.
    decoded = miniaudio.decode(source.read_bytes(), output_format=miniaudio.SampleFormat.FLOAT32,
                              nchannels=1, sample_rate=48000)
    mono48 = np.asarray(decoded.samples, dtype=np.float32)
    mono = resample_poly(mono48, 3125, 3072).astype(np.float64)
    rms = float(np.sqrt(np.mean(mono * mono)))
    if not rms > 0:
        raise ValueError("Reference has zero RMS")
    # One fixed gain for the whole source preserves the original envelope.
    gain = 0.05 / rms
    normalized = mono * gain
    prefix, suffix = 1.0, 2.0
    ambient_rms = 0.05 * 10 ** (-40 / 20)
    work = ROOT / "build" / "reference_analysis"
    work.mkdir(parents=True, exist_ok=True)
    gen.rng = np.random.default_rng(188809)
    samples = gen.timeline_render([(prefix, normalized, args.azimuth)],
                                  prefix + len(mono) / gen.FS + suffix,
                                  amb_level=ambient_rms)
    packets, _ = gen.timeline_packets(samples)
    del samples
    packet_file = work / "reference_pkts.bin"
    packet_file.write_bytes(packets)
    del packets
    exe = work / "test_reference_doa.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(gen.GCC).parent) + os.pathsep + env.get("PATH", "")
    cmd = [gen.GCC, "-O2", "-std=gnu99", "-Wall", "-DSC_TARGET_ONLY=1", "-I", gen.SRC,
           str(ROOT / "sw/src/doa.c"), str(ROOT / "sw/src/sound_class.c"),
           str(ROOT / "sw/test/test_doa.c"), "-o", str(exe), "-lm"]
    subprocess.run(cmd, env=env, check=True)
    proc = subprocess.run([str(exe), str(packet_file), str(gen.NFR)], env=env,
                          capture_output=True, text=True, check=True)
    raw_path = work / "reference_features.txt"
    raw_path.write_text(proc.stdout, encoding="utf-8")
    rows = []
    for line in proc.stdout.splitlines():
        v = line.split()
        rows.append(dict(ret=int(v[1]), valid=int(v[2]), az=float(v[3]), ratio=float(v[4]),
                         energy=float(v[5]), noise=float(v[6]), coherence=float(v[7]),
                         peak=float(v[10]), tonal=float(v[11]), cls=int(v[12]),
                         event=int(v[13]), siren=int(v[14]), dir_ok=int(v[17])))
    dt = gen.NFR / gen.FS
    clip_start, clip_end = int(prefix / dt), int((prefix + len(mono) / gen.FS) / dt)
    selected = rows[clip_start:clip_end]
    # Reconstruct the same 48-block evidence window from exported features.
    # Exported values are rounded, so these checks are diagnostic approximations;
    # the siren-active output above is the actual C rule result.
    flags, window = [], []
    reasons = {"window_not_full": 0, "tonal_fraction_below_0.6": 0,
               "frequency_span_below_100Hz": 0, "energy_spread_above_8dB": 0}
    stats = []
    for idx, r in enumerate(rows):
        tonal = (r["tonal"] >= 12 and 400 <= r["peak"] <= 2000 and
                 r["energy"] - r["noise"] >= 8)
        flags.append(tonal)
        window.append(r if tonal else None)
        window = window[-48:]
        tone_rows = [x for x in window if x is not None]
        fraction = len(tone_rows) / len(window)
        if len(tone_rows) >= 4:
            hz = sorted(x["peak"] for x in tone_rows)
            levels = sorted(x["energy"] for x in tone_rows)
            n = len(tone_rows)
            span = hz[(n * 9) // 10] - hz[n // 10]
            spread = levels[(n * 9) // 10] - levels[n // 10]
        else:
            span, spread = 0.0, 100.0
        if clip_start <= idx < clip_end:
            stats.append(dict(time_s=round(idx * dt - prefix, 3), fraction=fraction,
                              span_hz=span, energy_spread_db=spread, active=r["siren"]))
            if not r["siren"]:
                reasons["window_not_full"] += len(window) < 48
                reasons["tonal_fraction_below_0.6"] += fraction < 0.6
                reasons["frequency_span_below_100Hz"] += span < 100
                reasons["energy_spread_above_8dB"] += spread > 8
    dropout = []
    for sec in range(int(len(mono) / gen.FS) + 1):
        segment = [x for x in stats if sec <= x["time_s"] < sec + 1]
        if segment:
            dropout.append(dict(start_s=sec,
                                active_fraction=round(float(np.mean([x["active"] for x in segment])), 4),
                                tone_fraction_median=round(float(np.median([x["fraction"] for x in segment])), 4),
                                span_hz_median=round(float(np.median([x["span_hz"] for x in segment])), 2),
                                energy_spread_db_median=round(float(np.median([x["energy_spread_db"] for x in segment])), 2)))
    active_mask = [bool(r["siren"]) for r in rows]
    result = {
        "scope": "Offline current C-rule compatibility diagnostic with synthesized ideal array propagation; no hardware/audio playback.",
        "identity_limit": "Ambulance label is supplied by user. This test does not distinguish ambulance from police/fire sirens or estimate real classification accuracy.",
        "source": {"path": str(source), "user_label": args.label,
                   "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                   "original_sample_rate_hz": info.sample_rate,
                   "original_channels": info.nchannels,
                   "duration_s": round(len(mono48) / 48000, 6)},
        "decode": {"format": "mono FLOAT32 at 48000 Hz via local miniaudio", "resample_ratio": "3125/3072",
                   "algorithm_sample_rate_hz": gen.FS, "input_rms": rms, "fixed_gain": gain,
                   "normalized_rms": 0.05, "normalized_peak": float(np.max(np.abs(normalized)))},
        "synthetic_setup": {"azimuth_deg": args.azimuth, "radius_m": gen.R,
                            "active_channels": [0, 1, 2, 3, 4, 5, 7], "empty_channel": 6,
                            "prefix_noise_s": prefix, "suffix_noise_s": suffix,
                            "independent_noise_rms": ambient_rms, "reflections": False,
                            "source_acoustic_response": "none; real phone/speaker/room response not modelled"},
        "rule_result": {"clip_blocks": len(selected), "sequence_error_blocks": sum(r["ret"] != 0 for r in rows),
                        "active_fraction_of_clip": float(np.mean([r["siren"] for r in selected])),
                        "direction_eligible_fraction_of_clip": float(np.mean([r["dir_ok"] for r in selected])),
                        "siren_active_intervals_relative_to_clip_s": intervals(active_mask, dt, prefix),
                        "nonactive_window_failures_overlapping_counts": reasons,
                        "approximation_note": "Failure reasons reconstruct rounded exported features; siren output is actual C code. Multiple conditions may fail in one block."},
        "feature_percentiles_during_clip": {key: percentiles([r[key] for r in selected])
                                           for key in ["peak", "tonal", "energy", "noise", "ratio", "coherence"]},
        "per_second_diagnostics": dropout,
        "raw_features": str(raw_path),
        "source_code_sha256": {name: hashlib.sha256((ROOT / "sw/src" / name).read_bytes()).hexdigest()
                               for name in ["doa.c", "sound_class.c", "doa_config.h"]},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(args.output), "source_seconds": result["source"]["duration_s"],
                      "active_fraction": result["rule_result"]["active_fraction_of_clip"],
                      "active_intervals": result["rule_result"]["siren_active_intervals_relative_to_clip_s"],
                      "failure_counts": reasons}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
