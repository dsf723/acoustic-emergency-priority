"""Read-only audio-chain diagnostic; never treats failed matching as proof of no playback."""
from pathlib import Path
import argparse
import hashlib
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/python_deps"))
import miniaudio
import numpy as np
from scipy import signal


def db(x):
    return float(20 * np.log10(max(float(x), 1e-20)))


def top_peaks(x, fs):
    f, p = signal.welch(x, fs, nperseg=4096)
    select = (f >= 200) & (f <= 2500)
    peaks, _ = signal.find_peaks(p)
    peaks = [i for i in peaks if select[i]]
    peaks = sorted(peaks, key=lambda i: p[i], reverse=True)[:10]
    return [{"hz": round(float(f[i]), 2), "power_db": round(float(10 * np.log10(p[i] + 1e-30)), 2)}
            for i in peaks]


def normalized_matching(reference, capture, fs):
    sos = signal.butter(4, [400, 2400], btype="bandpass", fs=fs, output="sos")
    ref = signal.sosfiltfilt(sos, reference)
    rec = signal.sosfiltfilt(sos, capture)
    corr = signal.correlate(ref, rec, mode="valid", method="fft")
    cs = np.r_[0, np.cumsum(ref * ref)]
    energy = cs[len(rec):] - cs[:-len(rec)]
    ncc = corr / np.sqrt(np.maximum(energy * np.dot(rec, rec), 1e-30))
    best = int(np.argmax(np.abs(ncc)))
    # Short waveform matches tolerate clock drift, at the cost of more false
    # candidates for repetitive tones. No hard pass/fail identity threshold.
    short = []
    for sec in [1, 4, 8, 12]:
        part = rec[int(sec * fs):int((sec + 1) * fs)]
        c = signal.correlate(ref, part, mode="valid", method="fft")
        e = cs[len(part):] - cs[:-len(part)]
        values = c / np.sqrt(np.maximum(e * np.dot(part, part), 1e-30))
        # A digital-silence reference window has undefined NCC. Exclude it;
        # tiny cumulative-sum cancellation otherwise produces scores > 1.
        values[e < np.max(e) * 1e-8] = 0.0
        k = int(np.argmax(np.abs(values)))
        short.append({"capture_start_s": sec, "best_reference_start_s": round(k / fs, 4),
                      "absolute_ncc": round(float(abs(values[k])), 5)})
    return {"whole_capture_best_reference_start_s": round(best / fs, 4),
            "whole_capture_absolute_ncc": round(float(abs(ncc[best])), 6),
            "one_second_windows": short}


def spectral_matching(reference, capture, fs):
    def make(x):
        f, t, z = signal.stft(x, fs, nperseg=512, noverlap=384, boundary=None)
        keep = (f >= 400) & (f <= 2400)
        q = np.log(np.abs(z[keep]) ** 2 + 1e-16)
        # Remove static speaker/room coloration independently in each band,
        # then compare the time-varying spectral pattern.
        return q - np.mean(q, axis=1, keepdims=True)
    a, b = make(reference), make(capture)
    full = sum(signal.correlate(x, y, mode="valid", method="fft") for x, y in zip(a, b))
    cs = np.r_[0, np.cumsum(np.sum(a * a, axis=0))]
    energies = cs[b.shape[1]:] - cs[:-b.shape[1]]
    corr = full / np.sqrt(np.maximum(energies * np.sum(b * b), 1e-30))
    k = int(np.argmax(corr))
    return {"best_reference_start_s": round(k * 128 / fs, 4),
            "normalized_dynamic_log_spectral_correlation": round(float(corr[k]), 6),
            "method": "400-2400 Hz STFT log power; each frequency band mean removed; no trained decision threshold"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("reference", type=Path)
    ap.add_argument("capture", type=Path)
    ap.add_argument("--output", type=Path, default=ROOT / "build/reference_capture_comparison.json")
    args = ap.parse_args()
    ref = miniaudio.decode(args.reference.read_bytes(), output_format=miniaudio.SampleFormat.FLOAT32,
                           nchannels=1, sample_rate=48000)
    reference = signal.resample_poly(np.asarray(ref.samples, dtype=float), 1, 6)
    data = np.load(args.capture)
    raw = data["samples"].reshape(-1, 8).astype(float) / 2 ** 23
    channel_rms = np.sqrt(np.mean((raw - raw.mean(axis=0)) ** 2, axis=0))
    out = {
        "reference": {"path": str(args.reference), "sha256": hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                      "duration_s": len(reference) / 8000, "spectral_peaks": top_peaks(reference, 8000)},
        "capture": {"path": str(args.capture), "duration_s": len(raw) / 48828.125,
                    "block_gaps": int(np.sum(np.diff(data["blkno"].astype(np.int64)) != 1)),
                    "channel_rms_dbfs": [round(db(v), 2) for v in channel_rms],
                    "channel_peak_dbfs": [round(db(v), 2) for v in np.max(np.abs(raw), axis=0)]},
        "channels": {},
        "limits": ["Microphone channel level and source file level are not acoustically calibrated.",
                   "Whole-waveform correlation is sensitive to phone clock drift, speaker filtering and reverberation.",
                   "A low match cannot prove which phone file was played or prove no playback.",
                   "Repetitive siren tones can produce ambiguous one-second matches.",
                   "No ambulance/police/fire identity is established by this comparison."],
    }
    for ch in [0, 7]:
        x = signal.resample_poly(raw[:, ch], 512, 3125)
        sos = signal.butter(4, [400, 2400], btype="bandpass", fs=8000, output="sos")
        band = signal.sosfiltfilt(sos, x)
        parts = [band[i:i + 8000] for i in range(0, len(band), 8000) if len(band[i:i + 8000]) >= 4000]
        rms = np.array([np.sqrt(np.mean(q * q)) for q in parts])
        out["channels"][str(ch)] = {
            "spectral_peaks": top_peaks(x, 8000),
            "band_400_2400_rms_dbfs_per_second": [round(db(v), 2) for v in rms],
            "p90_to_p10_band_level_difference_db": round(db(np.percentile(rms, 90) / np.percentile(rms, 10)), 2),
            "waveform_matching": normalized_matching(reference, x, 8000),
            "spectral_matching": spectral_matching(reference, x, 8000),
        }
    args.output.write_text(json.dumps(out, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(out, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
