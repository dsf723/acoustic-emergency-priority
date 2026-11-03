"""
方向实测评估：同一段录音同时给出
  1. 显示方向：与板上相同的 C 代码（聚焦定位 + 目标输出）重放后，判为警笛且方向有效的块的方位；
  2. 直达声方向：警笛每次换音时，新音的直达声比墙面反射先到 1~10ms。对新音频率做窄带复解调，
     取起音后 0~3ms 的 7 麦相位做波束扫描（一致度 1 表示完全符合单一平面波）。
两者都偏离标注 → 声源实际位置或阵列安装方向与标注不符；只有显示方向偏 → 房间反射（窄带稳态声的视在方向偏移）。

用法：
  python dir_eval.py raw.npz [--truth 度] [--background 背景raw.npz]   离线评估一段录音
  python dir_eval.py --arm                                              经 JTAG 待触发录音（声音出现前保留约 1 秒）
  python dir_eval.py --fetch 输出.npz --truth 度 [--note 说明]          读出录音、保存标注并评估
  python dir_eval.py --record 输出.npz --truth 度 [--background 背景]   警笛已在播放时：立即录约 16 秒、读出并评估
方位约定与 doa_config.h 相同：0° 指向 0 号麦（北口），逆时针为正（90° 西、180° 南、270° 东）。
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "sw", "test"))
import gen_test as g  # noqa: E402
import interference_test as it  # noqa: E402

FS, C, R = 48828.125, 343.0, 0.040
CH = [0, 1, 2, 3, 4, 5, 7]


def approach(az):
    """与 traffic_light.c 的 tl_approach_from_az 相同的四分区"""
    a = az % 360
    return "北" if a < 45 or a >= 315 else "西" if a < 135 else "南" if a < 225 else "东"


def angerr(a, b):
    return (a - b + 180) % 360 - 180


def circ_stats(deg):
    v = np.exp(1j * np.deg2rad(np.asarray(deg)))
    m = np.mean(v)
    return float(np.rad2deg(np.angle(m)) % 360), float(np.rad2deg(np.sqrt(max(-2 * np.log(max(abs(m), 1e-9)), 0))))


def replay_display(samples, background=None):
    """C 代码重放，返回 (每块是否判为警笛, 显示方向有效的块的方位列表)"""
    exe, env = it.build(1)
    warm = 0
    if background is not None:
        samples = np.concatenate((background[-96:], samples))
        warm = min(96, len(background))
    rows = it.run(exe, env, b"".join(g.pack(samples[i].T, i * g.NFR) for i in range(len(samples))))[warm:]
    active = np.array([r["active"] for r in rows])
    shown = [r["az"] for r in rows if r["active"] and r["dvalid"]]
    return active, shown


def tones(x, n=2):
    """录音中最强的 n 个窄带音（1000~3000Hz，相距 > 200Hz）"""
    fr = np.lib.stride_tricks.sliding_window_view(x[:, 0], 2048)[::1024] * np.hanning(2048)
    p = np.median(np.abs(np.fft.rfft(fr, axis=1)) ** 2, axis=0)
    f = np.fft.rfftfreq(2048, 1 / FS)
    band = np.where((f > 1000) & (f < 3000))[0]
    pk, _ = signal.find_peaks(p[band])
    out = []
    for k in band[pk][np.argsort(p[band][pk])[::-1]]:
        if all(abs(f[k] - o) > 200 for o in out):
            out.append(float(f[k]))
        if len(out) == n:
            break
    return out


def onset_directions(x, freq, bw=150.0, min_coh=0.85):
    """返回 [(起音时刻 s, 起音后 0~3ms 方向, 一致度, 稳态 100~300ms 方向)]"""
    ang = np.deg2rad([60 * k for k in range(6)])
    pos = np.r_[np.c_[R * np.cos(ang), R * np.sin(ang)], [[0, 0]]]
    az = np.arange(360)
    U = np.c_[np.cos(np.deg2rad(az)), np.sin(np.deg2rad(az))]
    A = np.exp(-2j * np.pi * freq * (-(pos @ U.T) / C))
    nn = np.arange(len(x))
    h = np.hanning(int(FS / bw) | 1)
    z = signal.fftconvolve(x * np.exp(-2j * np.pi * freq * nn / FS)[:, None], (h / h.sum())[:, None], mode="same", axes=0)
    env_db = 20 * np.log10(np.mean(np.abs(z), axis=1) + 1e-9)
    w5 = int(0.005 * FS)
    sm = signal.fftconvolve(env_db, np.ones(w5) / w5, mode="same")
    lag = int(0.015 * FS)
    cand = np.where(sm[lag:] - sm[:-lag] > 8)[0]

    def doa(zt):
        p = zt / (np.abs(zt) + 1e-30)
        srp = np.abs(A.conj().T @ p) ** 2 / len(p) ** 2
        k = int(np.argmax(srp))
        return k, float(srp[k])

    out, last = [], -10 ** 9
    for o in cand:
        if o - last < int(0.3 * FS) or o + lag + int(0.4 * FS) >= len(x):
            continue
        last = o
        plateau = np.median(env_db[o + lag + int(0.05 * FS): o + lag + int(0.3 * FS)])
        idx = np.where(env_db[o:o + lag + int(0.03 * FS)] > plateau - 12)[0]
        if not len(idx):
            continue
        t0 = o + idx[0]
        early = [doa(z[t0 + k * int(0.00025 * FS)]) for k in range(12)]
        coh = float(np.mean([q for _, q in early]))
        if coh < min_coh:
            continue
        late = [doa(z[t0 + int(0.1 * FS) + k * int(0.002 * FS)])[0] for k in range(100)]
        out.append((t0 / FS, circ_stats([k for k, _ in early])[0], coh, circ_stats(late)[0]))
    return out


def evaluate(path, truth=None, background=None):
    with np.load(path) as d:
        samples = d["samples"]
        bn = d["blkno"]
    gaps = int(np.sum(np.diff(bn.astype(np.int64)) != 1))
    bg = None
    if background:
        with np.load(background) as d:
            bg = d["samples"]
    active, shown = replay_display(samples, bg)
    x = samples[:, :, CH].reshape(-1, len(CH)).astype(np.float64)
    x -= x.mean(axis=0)
    res = {"file": os.path.relpath(path, ROOT), "seconds": round(len(samples) * g.NFR / FS, 1),
           "blocks": int(len(samples)), "block_gaps": gaps, "truth_deg": truth,
           "siren_blocks": int(active.sum()), "shown_blocks": len(shown)}
    print(f"{res['file']}：{res['blocks']} 块（{res['seconds']} s），块号间断 {gaps} 处" +
          (f"，标注 {truth:.0f}°（{approach(truth)}口）" if truth is not None else ""))
    if shown:
        m, sd = circ_stats(shown)
        res.update(shown_mean_deg=round(m, 1), shown_spread_deg=round(sd, 1),
                   shown_approach={k: int(sum(approach(a) == k for a in shown)) for k in "北西南东"})
        line = f"  显示方向：判为警笛 {active.sum()} 块，显示方向 {len(shown)} 块，圆均值 {m:.1f}°（圆标准差 {sd:.1f}°），进口 {res['shown_approach']}"
        if truth is not None:
            e = np.abs([angerr(a, truth) for a in shown])
            res.update(shown_err_median=round(float(np.median(e)), 1), shown_err_p90=round(float(np.percentile(e, 90)), 1),
                       shown_correct_approach=round(float(np.mean([approach(a) == approach(truth) for a in shown])), 3))
            line += f"；误差中位数 {res['shown_err_median']}°、P90 {res['shown_err_p90']}°，进口正确 {res['shown_correct_approach'] * 100:.0f}%"
        print(line)
    else:
        print(f"  显示方向：判为警笛 {active.sum()} 块，没有显示方向的块")
    res["onsets"] = {}
    for f in tones(x):
        on = onset_directions(x, f)
        if not on:
            print(f"  直达声 {f:.0f}Hz：没有一致度 ≥0.85 的起音")
            continue
        em, es = circ_stats([o[1] for o in on])
        lm, ls = circ_stats([o[3] for o in on])
        coh = float(np.mean([o[2] for o in on]))
        res["onsets"][f"{f:.0f}"] = dict(n=len(on), onset_mean_deg=round(em, 1), onset_spread_deg=round(es, 1),
                                          coherence=round(coh, 3), steady_mean_deg=round(lm, 1), steady_spread_deg=round(ls, 1))
        line = (f"  直达声 {f:.0f}Hz：{len(on)} 次起音，起音后 0~3ms {em:.1f}°（圆标准差 {es:.1f}°，一致度 {coh:.2f}）；"
                f"同一音 100~300ms 后 {lm:.1f}°（圆标准差 {ls:.1f}°）")
        if truth is not None:
            line += f"；起音方向误差 {angerr(em, truth):+.1f}°"
        print(line)
    return res


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("npz", nargs="?")
    ap.add_argument("--truth", type=float)
    ap.add_argument("--background")
    ap.add_argument("--arm", action="store_true")
    ap.add_argument("--fetch")
    ap.add_argument("--record")
    ap.add_argument("--note", default="")
    a = ap.parse_args()
    rd = os.path.join(HERE, "rec_dump.py")
    if a.arm:
        subprocess.run([sys.executable, "-I", rd, "--arm", "15"], check=True)
        return 0
    if a.record:                            # 警笛已在播放：立即连续录约 16 秒
        subprocess.run([sys.executable, "-I", rd, "--continuous"], check=True)
        time.sleep(17)
        a.fetch = a.record
    if a.fetch:
        subprocess.run([sys.executable, "-I", rd, a.fetch], check=True)
        res = evaluate(a.fetch, a.truth, a.background)
        res.update(note=a.note, fetched_at=datetime.datetime.now().isoformat(timespec="seconds"))
        with open(os.path.splitext(a.fetch)[0] + ".json", "w", encoding="utf-8") as fp:
            json.dump(res, fp, ensure_ascii=False, indent=2)
        return 0
    if not a.npz:
        ap.error("需要录音文件，或 --arm / --fetch / --record")
    evaluate(a.npz, a.truth, a.background)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
