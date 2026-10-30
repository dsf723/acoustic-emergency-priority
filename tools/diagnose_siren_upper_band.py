"""Offline copied-source 2000/2500 Hz parameter comparison. No firmware writes."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/diagnostic_siren_upper_band"
SPEC = importlib.util.spec_from_file_location("diag_gen", ROOT / "sw/test/gen_test.py")
g = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(g)
TARGET = ROOT / "rec/dataset/ambulance/20261019T141016_786958Z_zhipianbang_188809_78651b9c/raw.npz"
BACKGROUND = ROOT / "rec/dataset/background/20261019T140447_316549Z_room_background_20261019_fc230556/raw.npz"


def packets(npz):
    s = np.load(npz)["samples"]
    return b"".join(g.pack(s[i].T, i * g.NFR) for i in range(len(s)))


def run(exe, packet_file, offset_blocks, upper):
    proc = subprocess.run([str(exe), str(packet_file), str(g.NFR)], capture_output=True,
                          text=True, env=ENV, check=True)
    rows = []
    for line in proc.stdout.splitlines():
        a = line.split()
        rows.append({"energy": float(a[5]), "noise": float(a[6]), "peak": float(a[10]),
                     "tonal": float(a[11]), "active": int(a[14]), "dir_ok": int(a[17])})
    (WORK / (packet_file.stem + "_" + str(upper) + "_features.txt")).write_text(proc.stdout)
    selected = rows[offset_blocks:]
    flags = [r["tonal"] >= 12 and 400 <= r["peak"] <= upper and r["energy"] - r["noise"] >= 8 for r in rows]
    stats = []
    for i in range(offset_blocks, len(rows)):
        ix = [j for j in range(max(0, i - 47), i + 1) if flags[j]]
        hz = sorted(rows[j]["peak"] for j in ix)
        energy = sorted(rows[j]["energy"] for j in ix)
        n = len(ix)
        span = hz[n * 9 // 10] - hz[n // 10] if n >= 4 else 0
        spread = energy[n * 9 // 10] - energy[n // 10] if n >= 4 else 100
        stats.append((n / min(i + 1, 48), span, spread))
    starts, active_ranges = None, []
    for i, r in enumerate(selected + [{"active": 0}]):
        if r["active"] and starts is None: starts = i
        if not r["active"] and starts is not None:
            active_ranges.append([round(starts * g.NFR / g.FS, 3), round(i * g.NFR / g.FS, 3)])
            starts = None
    return {"blocks": len(selected), "active_blocks": sum(r["active"] for r in selected),
            "active_intervals_relative_to_target_start_s": active_ranges,
            "eligible_tonal_fraction": float(np.mean(flags[offset_blocks:])),
            "best_one_second_tonal_fraction": max(a[0] for a in stats),
            "window_span_hz_p10_50_90": np.percentile([a[1] for a in stats], [10, 50, 90]).tolist(),
            "window_energy_spread_db_p10_50_90": np.percentile([a[2] for a in stats], [10, 50, 90]).tolist(),
            "eligible_direction_fraction": float(np.mean([r["dir_ok"] for r in selected])),
            "snr_db_p10_50_90": np.percentile([r["energy"] - r["noise"] for r in selected], [10, 50, 90]).tolist()}


if __name__ == "__main__":
    WORK.mkdir(parents=True, exist_ok=True)
    ENV = dict(os.environ)
    ENV["PATH"] = str(Path(g.GCC).parent) + os.pathsep + ENV.get("PATH", "")
    bg_data, target_data = packets(BACKGROUND), packets(TARGET)
    bg_path, warm_path = WORK / "background.bin", WORK / "background_then_second_target.bin"
    bg_path.write_bytes(bg_data)
    warm_path.write_bytes(bg_data + target_data)
    # Existing synthetic negatives are a narrow regression only, not substitute
    # for real police/fire/horn/music recordings.
    g.rng = np.random.default_rng(7020)
    negative_sources = [(2.0, g.clap_sig(0.015), 30.0),
                        (5.0, g.tone_sig(lambda t: 1500 + 0 * t, 3.0, 0.05, 1), 45.0),
                        (10.0, g.speech_sig(4.0, 0.05), 200.0),
                        (16.0, 0.05 * g.rng.standard_normal(int(2.0 * g.FS)), 10.0),
                        (20.0, g.music_sig(4.0, 0.2), 330.0)]
    negative_audio = g.timeline_render(negative_sources, 26.0, amb_level=0.05 * 10 ** (-38 / 20))
    negative_data, _ = g.timeline_packets(negative_audio)
    del negative_audio
    negative_path = WORK / "synthetic_negatives.bin"
    negative_path.write_bytes(negative_data)
    del negative_data
    hashes = {f: hashlib.sha256((ROOT / "sw/src" / f).read_bytes()).hexdigest()
              for f in ["doa.c", "sound_class.c", "doa_config.h"]}
    result = {"scope": "Copied C sources only; no actual firmware change/build/download. Single-source diagnostic, not ambulance classification accuracy.",
              "warmup": "entire independent 16.106s background recording before target", "baseline_source_sha256": hashes,
              "real_target": str(TARGET), "real_background": str(BACKGROUND), "variants": {}}
    for upper in (2000, 2500):
        local = WORK / str(upper)
        local.mkdir(exist_ok=True)
        for name in ["doa.c", "doa.h", "sound_class.c", "sound_class.h", "doa_config.h"]:
            shutil.copy2(ROOT / "sw/src" / name, local / name)
        config = (local / "doa_config.h").read_text(encoding="utf-8")
        config, changed = re.subn(r"(?m)^#define\s+SC_SIREN_F_HI_HZ\s+\S+",
                                  f"#define SC_SIREN_F_HI_HZ        {upper}.0f", config)
        if changed != 1:
            raise RuntimeError("Expected exactly one upper-band definition")
        (local / "doa_config.h").write_text(config, encoding="utf-8")
        shutil.copy2(ROOT / "sw/test/test_doa.c", local / "test_doa.c")
        exe = local / "diagnostic_doa.exe"
        subprocess.run([g.GCC, "-O2", "-std=gnu99", "-Wall", "-DSC_TARGET_ONLY=1", "-I", str(local),
                        str(local / "doa.c"), str(local / "sound_class.c"), str(local / "test_doa.c"),
                        "-o", str(exe), "-lm"], env=ENV, check=True)
        variant = {"background_only": run(exe, bg_path, 0, upper),
                   "second_target_after_background": run(exe, warm_path, 768, upper),
                   "synthetic_negatives": run(exe, negative_path, 0, upper)}
        ref_path = ROOT / "build/reference_analysis/reference_pkts.bin"
        if ref_path.is_file():
            variant["synthetic_reference"] = run(exe, ref_path, int(g.FS / g.NFR), upper)
        result["variants"][str(upper)] = variant
        print(json.dumps({str(upper): variant}, indent=2))
        (WORK / "comparison.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
