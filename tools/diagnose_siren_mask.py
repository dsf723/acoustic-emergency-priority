"""Temporary copied-source direction/mask checks; no hardware or live source edits."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/diagnostic_siren_upper_band"
GCC = Path(r"D:\Xilinx\Vivado\2018.3\tps\mingw\6.2.0\win64.o\nt\bin\gcc.exe")
env = dict(os.environ)
env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
results = {}
for minimum in (6, 3, 2):
    local = WORK / ("mask_" + str(minimum))
    local.mkdir(exist_ok=True)
    for name in ["doa.c", "doa.h", "sound_class.c", "sound_class.h", "doa_config.h", "test_doa.c"]:
        shutil.copy2(WORK / "2500" / name, local / name)
    config = (local / "doa_config.h").read_text(encoding="utf-8")
    config = config.replace("#define DOA_MASK_MIN_BINS   6", f"#define DOA_MASK_MIN_BINS   {minimum}")
    (local / "doa_config.h").write_text(config, encoding="utf-8")
    doa = (local / "doa.c").read_text(encoding="utf-8")
    doa = "int diagnostic_raw_nused;\n" + doa
    doa = doa.replace("if (nused < DOA_MASK_MIN_BINS)", "diagnostic_raw_nused = nused;\n    if (nused < DOA_MASK_MIN_BINS)")
    (local / "doa.c").write_text(doa, encoding="utf-8")
    test = (local / "test_doa.c").read_text(encoding="utf-8")
    test = "extern int diagnostic_raw_nused;\n" + test
    test = test.replace("%.1f %.2f %.2f\\n\",", "%.1f %.2f %.2f %d %d\\n\",")
    test = test.replace("r.peak_level_db, r.crest_db);", "r.peak_level_db, r.crest_db, r.nbins, diagnostic_raw_nused);")
    (local / "test_doa.c").write_text(test, encoding="utf-8")
    exe = local / "diagnostic_doa.exe"
    subprocess.run([str(GCC), "-O2", "-std=gnu99", "-Wall", "-DSC_TARGET_ONLY=1", "-I", str(local),
                    str(local / "doa.c"), str(local / "sound_class.c"), str(local / "test_doa.c"),
                    "-o", str(exe), "-lm"], env=env, check=True)
    proc = subprocess.run([str(exe), str(WORK / "background_then_second_target.bin"), "1024"],
                          env=env, capture_output=True, text=True, check=True)
    (local / "features.txt").write_text(proc.stdout)
    rows = [list(map(float, line.split())) for line in proc.stdout.splitlines()][768:]
    a = np.asarray(rows)
    active = a[:, 14] > 0
    eligible = a[:, 17] > 0
    error = np.abs((a[:, 3] + 180) % 360 - 180)
    coh = a[:, 7]
    def selected(mask):
        return {"blocks": int(mask.sum()),
                "within_20deg_of_operator_0_fraction": float(np.mean(error[mask] <= 20)) if mask.any() else None,
                "angular_error_p10_50_90": np.percentile(error[mask], [10, 50, 90]).tolist() if mask.any() else [],
                "coherence_p10_50_90": np.percentile(coh[mask], [10, 50, 90]).tolist() if mask.any() else []}
    results[str(minimum)] = {"upper_band_hz": 2500, "raw_mask_nused_p10_50_90": np.percentile(a[:, 22], [10, 50, 90]).tolist(),
                             "fallback_to_all_bins_fraction": float(np.mean(a[:, 22] < minimum)),
                             "final_used_bins_p10_50_90": np.percentile(a[:, 21], [10, 50, 90]).tolist(),
                             "all_blocks": selected(np.ones(len(a), dtype=bool)),
                             "active_siren_blocks": selected(active), "direction_eligible_blocks": selected(eligible)}
    print(json.dumps({str(minimum): results[str(minimum)]}, indent=2))
(WORK / "mask_comparison.json").write_text(json.dumps({"scope": "Copied-source diagnostic only; operator 0 degree is unverified physical label, not forced output", "results": results}, indent=2))
