"""
把 rec_dump.py 读出的连续录音交给 PC 版算法（与板上相同的 doa.c + sound_class.c）重放，
打印识别事件与响亮块的特征，用于标注数据的规则调整与远近阈值标定。

用法：python analyze_rec.py rec.npz [--all]     --all 打印所有响亮块的特征
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sw", "test"))
import gen_test as g  # noqa: E402

NAMES = {0: "-", 1: "SOUND", 2: "CLAP", 3: "SIREN"}


def replay(path, allow_gaps=False, background=None):
    d = np.load(path)
    s, bn = d["samples"], d["blkno"]
    gaps = np.where(np.diff(bn.astype(np.int64)) != 1)[0]
    if len(gaps) and not allow_gaps:
        raise ValueError(f"录音有 {len(gaps)} 处块号间断，不能拼接后评估时序识别。请用 rec_dump.py --continuous 重新录制；"
                         "仅调试特征时可显式加 --allow-gaps（识别事件和压缩后的时间不可作验收依据）。")
    exe, env = g.build(target_only=True)
    warmup = 0
    if background is not None:
        with np.load(background, allow_pickle=False) as bg:
            bs, bn_bg = bg['samples'], bg['blkno']
        if np.any(np.diff(bn_bg.astype(np.int64)) != 1):
            raise ValueError('背景预段块号有间断，不能用于初始化噪声估计')
        bs = bs[-96:]  # at most about two seconds of separately captured background
        warmup = len(bs)
        s = np.concatenate((bs, s), axis=0)
    data = b"".join(g.pack(s[i].T, i * g.NFR) for i in range(len(s)))
    return g.run(exe, env, data)[warmup:], gaps


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    path = sys.argv[1]
    background = sys.argv[sys.argv.index('--background') + 1] if '--background' in sys.argv else None
    rows, gaps = replay(path, allow_gaps='--allow-gaps' in sys.argv, background=background)
    if background:
        print('独立背景预段用于初始化噪声估计；这是离线状态初始化，不是连续实录，以下时间以目标录音起点计。')
    if len(gaps):
        print('WARNING: gaps were explicitly allowed; stateful detections and compressed timestamps are not valid measurements.')
    bt = g.NFR / g.FS
    print(f"{path}: {len(rows)} 块（{len(rows) * bt:.1f}s），块号不连续处 {len(gaps)} 个")
    print("识别事件：")
    for i, r in enumerate(rows):
        if r["event"]:
            print(f"  t={i * bt:6.2f}s {NAMES[r['cls']]:5s} 方位 {r['event_az'] if r['cls'] == 2 else r['az']:6.1f}°"
                  f"  远近档位 {r['band']}（0=未知）  峰值电平 {r['plevel']:6.1f}dBFS  峰均比 {r['crest']:4.1f}")
    siren = [r["siren"] for r in rows]
    print(f"判为警笛的块：{sum(siren)} / {len(rows)}")
    if "--all" in sys.argv:
        print("  时间     E     SNR    PL    峰均比 平坦  质心  谱峰  音调  峰值比  方位  类别")
        for i, r in enumerate(rows):
            if r["e"] - r["noise"] > 10:
                print(f"  {i * bt:6.2f} {r['e']:6.1f} {r['e'] - r['noise']:5.1f} {r['plevel']:6.1f} {r['crest']:5.1f} "
                      f"{r['flat']:.2f} {r['cent']:5.0f} {r['peak']:5.0f} {r['tonal']:5.1f} {r['ratio']:5.2f} "
                      f"{r['az']:5.1f} {NAMES[r['cls']]}")


if __name__ == "__main__":
    main()
