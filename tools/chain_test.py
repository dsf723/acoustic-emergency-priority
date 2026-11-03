"""
全链路计时：警笛出现 → 识别确认 → 方向确认 → 信号灯进入优先 → 来车进口变绿。
板上 main.c 的 ct_* 变量记录各事件第一次发生的块号与毫秒时刻；同时待触发录音（保留声音出现前约 1 秒），
用录音核对"声音出现"的时刻。

用法（开发板运行 sound_loc.elf、JTAG 已连接）：
  python chain_test.py --arm                 开始一次测量（之后再开始放警笛）
  python chain_test.py --status              查看已记录到哪一步
  python chain_test.py --read 输出.npz [--truth 度] [--note 说明]   读出结果与录音，保存 .json
"""
import argparse
import datetime
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rec_dump as rd  # noqa: E402

FS = 48828.125
BLOCK_MS = 1024 / FS * 1000
NAMES = ["声音出现", "警笛确认", "方向确认", "信号灯进入优先", "来车进口变绿"]
APPROACH = ["北", "南", "东", "西"]
STATE = ["绿", "黄", "全红"]


def read_ct(syms):
    a = {n: rd.addr(syms, n) for n in ("ct_arm", "ct_blk", "ct_ms", "ct_info")}
    out = rd.run_tcl(f'stop\nafter 50\nputs "CT [mrd -force -value {a["ct_arm"]} 1] | [mrd -force -value {a["ct_blk"]} 5] | '
                     f'[mrd -force -value {a["ct_ms"]} 5] | [mrd -force -value {a["ct_info"]} 3]"\ncon\n')
    line = [l for l in out.splitlines() if l.startswith("CT ")][0][3:]
    arm, blk, ms, info = (p.split() for p in line.split("|"))
    s32 = lambda v: int(v) - (1 << 32) if int(v) >= 1 << 31 else int(v)
    return int(arm[0]), [int(v) for v in blk], [int(v) for v in ms], [s32(v) for v in info]


def report(arm, blk, ms, info):
    if arm == 0:
        print("还没有开始测量（先 --arm）")
        return {}
    if not blk[0]:
        print("已开始，尚未检测到声音")
        return {}
    ph, st = info[2] >> 8, info[2] & 0xFF
    res = {"start_phase": "南北" if ph == 0 else "东西", "start_state": STATE[st] if 0 <= st < 3 else str(st), "events": {}}
    print(f"声音出现时信号灯：{res['start_phase']}相位{res['start_state']}灯")
    for i, n in enumerate(NAMES):
        if blk[i]:
            dt = (ms[i] - ms[0]) / 1000
            res["events"][n] = {"blk": blk[i], "ms": ms[i], "t_s": round(dt, 2)}
            extra = ""
            if i == 2:
                appr = APPROACH[info[1]] if 0 <= info[1] < 4 else "?"
                res.update(dir_az_deg=info[0] / 10, dir_approach=appr)
                extra = f"（方位 {info[0] / 10:.1f}°，{appr}口）"
            print(f"  {n:<8} +{dt:6.2f} s  块 {blk[i]}{extra}")
        else:
            print(f"  {n:<8}   未发生")
    if blk[1] and blk[4]:
        green_before = (res["dir_approach"] in "北南") == (res["start_phase"] == "南北") and res["start_state"] == "绿"
        res["case"] = "绿灯延长" if green_before else "红灯早断"
        print(f"  情形：来车进口在声音出现时{'是绿灯（绿灯延长）' if green_before else '不是绿灯（红灯早断）'}；"
              f"识别确认后 {(ms[4] - ms[1]) / 1000:.2f} s 变绿，全程 {(ms[4] - ms[0]) / 1000:.2f} s")
    return res


def audio_onset(npz, ct_blk0):
    """录音里 1000~3000Hz 能量首次比开头 0.5 秒高 15dB 的块，相对板上"声音出现"块的时间差（秒）"""
    with np.load(npz) as d:
        s, bn = d["samples"], d["blkno"]
    x = s[:, :, 0].astype(np.float64)
    X = np.abs(np.fft.rfft((x - x.mean(axis=1, keepdims=True)) * np.hanning(x.shape[1]), axis=1)) ** 2
    f = np.fft.rfftfreq(x.shape[1], 1 / FS)
    e = 10 * np.log10(X[:, (f > 1000) & (f < 3000)].sum(axis=1) + 1e-9)
    k = np.where(e > np.median(e[:24]) + 15)[0]
    if not len(k):
        return None
    return int(bn[k[0]]), (int(bn[k[0]]) - ct_blk0) * BLOCK_MS / 1000


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arm", action="store_true")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--read")
    ap.add_argument("--truth", type=float)
    ap.add_argument("--note", default="")
    a = ap.parse_args()
    syms = rd.symbols()
    if a.arm:
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "rec_dump.py"), "--arm", "15"], check=True)
        rd.run_tcl(f"stop\nmwr -force {rd.addr(syms, 'ct_arm')} 1\ncon\n")
        print("已开始：现在放警笛，等来车方向变绿后再读出")
        return 0
    arm, blk, ms, info = read_ct(syms)
    if a.status or not a.read:
        report(arm, blk, ms, info)
        return 0
    res = report(arm, blk, ms, info)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "rec_dump.py"), a.read], check=True)
    if blk[0]:
        on = audio_onset(a.read, blk[0])
        if on:
            res["audio_onset_blk"], res["audio_onset_vs_board_s"] = on[0], round(on[1], 3)
            print(f"  录音核对：声音在块 {on[0]} 出现，比板上记录早 {-on[1]:.2f} s" if on[1] < 0 else
                  f"  录音核对：声音在块 {on[0]} 出现，与板上记录相差 {on[1]:+.2f} s")
    res.update(truth_deg=a.truth, note=a.note, ct_arm=arm, ct_blk=blk, ct_ms=ms, ct_info=info,
               read_at=datetime.datetime.now().isoformat(timespec="seconds"))
    with open(os.path.splitext(a.read)[0] + ".json", "w", encoding="utf-8") as fp:
        json.dump(res, fp, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
