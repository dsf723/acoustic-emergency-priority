"""
用实测录音标定阵列几何：找出外圈 6 个通道在圆周上的真实排列，以及物理 0° 方向

用法：
  python calib_array.py rec.npz                      # 只找通道排列（相对方向）
  python calib_array.py rec.npz --dirs 0,90,180,270  # 拍手的实际方向（按时间顺序），同时求 0° 与旋向

方法：
  1. 在录音中找出响亮的瞬态事件（拍手），每个事件取能量最大的一块
  2. 对所有麦克风对做 GCC-PHAT（16 倍插值），得到实测时延
  3. 外圈 6 个位置（每 60° 一个）× 6 个通道的全部 720 种排列 × 若干半径，
     对每个事件搜索最佳方位，统计时延拟合残差，残差最小的排列即真实排列
  4. 给出 doa_config.h 应填写的 DOA_CH_OUTER / DOA_MIC0_ANGLE_DEG / DOA_MIC_DIR
"""
import argparse
import itertools
import sys

import numpy as np

FS = 48828.125
C = 343.0
CH_OUTER = [0, 1, 2, 3, 4, 5]
CH_CENTER = 7
F_LO, F_HI = 300.0, 6000.0
UPS = 16
MAXLAG = 16


def find_events(x, blkno, min_db, gap_blocks=8):
    """x: [块, 帧, 通道] float，blkno: 各块的块序号。返回事件块索引列表
    相邻块序号相差不超过 gap_blocks 的归为同一事件，取其中能量最大的一块"""
    n = x.shape[1]
    spec = np.fft.rfft((x - x.mean(axis=1, keepdims=True)) * np.hanning(n)[None, :, None], axis=1)
    f = np.fft.rfftfreq(n, 1 / FS)
    band = (f >= F_LO) & (f <= F_HI)
    chans = CH_OUTER + [CH_CENTER]
    e = 10 * np.log10(np.mean(np.abs(spec[:, band][:, :, chans]) ** 2, axis=(1, 2)) + 1e-30)
    floor = np.min(e)
    hot = np.where(e > floor + min_db)[0] if min_db > 0 else np.arange(len(e))
    events, group = [], []
    for b in hot:
        if group and blkno[b] - blkno[group[-1]] > gap_blocks:
            events.append(max(group, key=lambda i: e[i]))
            group = []
        group.append(b)
    if group:
        events.append(max(group, key=lambda i: e[i]))
    return events, e, floor


def gcc_tdoa(blk, chans):
    """blk: [帧, 8]。返回 dict (i,j)->tau（采样点，峰值位于 t_i - t_j）与峰值强度"""
    n = blk.shape[0]
    nfft = 2 * n
    X = np.fft.rfft((blk - blk.mean(axis=0)) * np.hanning(n)[:, None], nfft, axis=0)
    f = np.fft.rfftfreq(nfft, 1 / FS)
    band = (f >= F_LO) & (f <= F_HI)
    out = {}
    for i, j in itertools.combinations(range(len(chans)), 2):
        G = X[:, chans[i]] * np.conj(X[:, chans[j]])
        G = np.where(band, G / (np.abs(G) + 1e-30), 0)
        r = np.fft.irfft(G, nfft * UPS)
        r = np.concatenate([r[-MAXLAG * UPS:], r[:MAXLAG * UPS + 1]])
        k = np.argmax(r)
        out[(i, j)] = ((k - MAXLAG * UPS) / UPS, r[k] / band.sum())
    return out


def fit_events(tdoas, pos, az_grid):
    """pos: [7, 2]（米，顺序同 chans）。返回每个事件的 (最佳方位, 残差 RMS 采样点)"""
    pairs = list(tdoas[0].keys())
    u = np.stack([np.cos(az_grid), np.sin(az_grid)], axis=1)              # [A, 2]
    d = np.array([pos[j] - pos[i] for i, j in pairs]) * FS / C            # [P, 2]
    model = u @ d.T                                                       # [A, P]
    res = []
    for td in tdoas:
        meas = np.array([td[p][0] for p in pairs])
        err = np.sum((model - meas[None, :]) ** 2, axis=1)
        k = np.argmin(err)
        res.append((np.degrees(az_grid[k]) % 360, np.sqrt(err[k] / len(pairs))))
    return res


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("--dirs", help="拍手的实际方向（度，按时间顺序，逗号分隔）")
    ap.add_argument("--min-db", type=float, default=0.0,
                    help="只保留比录音中最弱块高多少 dB 的块（默认 0：录音本身已只含响亮块）")
    args = ap.parse_args()

    data = np.load(args.rec)
    samples = data["samples"].astype(np.float64)[:, 1:, :]                # 丢弃每块第 0 帧
    events, e, floor = find_events(samples, data["blkno"], args.min_db)
    print(f"录音 {samples.shape[0]} 块，最弱块能量 {floor:.1f} dB，"
          f"找到 {len(events)} 个事件：块 {events}")
    if len(events) < 3:
        print("事件太少（至少需要 3 次清脆的拍手），请重新录音。")
        return 1

    chans = CH_OUTER + [CH_CENTER]
    tdoas = [gcc_tdoa(samples[b], chans) for b in events]
    az_grid = np.radians(np.arange(0, 360, 0.5))

    best = None
    for R in np.arange(0.030, 0.0561, 0.0025):
        for perm in itertools.permutations(range(6)):
            if perm[0] != 0:            # 旋转对称：固定通道 0 在 0 号位置，其余排列与之等价
                continue
            pos = np.zeros((7, 2))
            for slot, chi in enumerate(perm):
                a = np.radians(60 * slot)
                pos[chi] = [R * np.cos(a), R * np.sin(a)]
            fit = fit_events(tdoas, pos, az_grid)
            score = np.median([r for _, r in fit])
            if best is None or score < best[0]:
                best = (score, R, perm, fit)

    score, R, perm, fit = best
    # 默认配置（doa_config.h 出厂值）的残差，作对比
    pos_def = np.zeros((7, 2))
    for k in range(6):
        a = np.radians(90 - 60 * k)
        pos_def[k] = [0.040 * np.cos(a), 0.040 * np.sin(a)]
    fit_def = fit_events(tdoas, pos_def, az_grid)

    print(f"\n默认配置残差（中位数）：{np.median([r for _, r in fit_def]):.3f} 采样点")
    print(f"最佳排列残差（中位数）：{score:.3f} 采样点，半径 {R * 1000:.1f} mm")
    slot_ch = [None] * 6
    for slot, chi in enumerate(perm):
        slot_ch[slot] = CH_OUTER[chi]
    print("最佳排列（逆时针每 60° 一个位置）对应通道：", slot_ch)
    for b, (az, r), (azd, rd) in zip(events, fit, fit_def):
        print(f"  块 {b:4d}  相对能量 {e[b] - floor:5.1f} dB  最佳排列下方位 {az:6.1f}°（残差 {r:.3f}）"
              f"  | 默认配置下 {azd:6.1f}°（残差 {rd:.3f}）")

    mic0, mdir = 0.0, 1
    if args.dirs:
        phys = [float(v) for v in args.dirs.split(",")]
        if len(phys) != len(events):
            print(f"\n--dirs 给了 {len(phys)} 个方向，但找到 {len(events)} 个事件，无法对应。"
                  "请调整 --min-db 或重新录音。")
        else:
            model = np.radians([az for az, _ in fit])
            ph = np.radians(phys)
            cands = []
            for d in (1, -1):
                a0 = np.angle(np.mean(np.exp(1j * (ph - d * model))))
                err = np.degrees(np.abs(np.angle(np.exp(1j * (ph - (a0 + d * model))))))
                cands.append((np.sqrt(np.mean(err ** 2)), d, np.degrees(a0) % 360, err))
            rms, mdir, mic0, err = min(cands, key=lambda c: c[0])
            print(f"\n与实际方向对比：旋向 DIR={mdir:+d}，0 号位置物理方位 {mic0:.1f}°，"
                  f"RMS 误差 {rms:.1f}°，各次误差 {np.round(err, 1).tolist()}")

    print("\n请在 sw/src/doa_config.h 中设置：")
    print(f"  #define DOA_ARRAY_RADIUS_M  {R:.4f}f")
    print(f"  #define DOA_MIC0_ANGLE_DEG  {mic0:.1f}f")
    print(f"  #define DOA_MIC_DIR         ({mdir:+d})")
    print(f"  #define DOA_CH_OUTER        {{ {', '.join(str(c) for c in slot_ch)} }}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
