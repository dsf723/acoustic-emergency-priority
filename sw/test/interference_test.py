"""
抗干扰回归：救护车警笛持续发声，同时有更响的干扰声源从其他方向出现，
检查显示方向是否始终指向警笛、不跟着干扰源变化。分别编译"聚焦定位"开 / 关两个版本对比。
用法：python interference_test.py
"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_test as g  # noqa: E402

SRC = g.SRC
SIREN_AZ = 120.0


def build(focus):
    exe = os.path.abspath(os.path.join(g.BUILD, f"test_target_doa_focus{focus}.exe"))
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(g.GCC) + os.pathsep + env["PATH"]
    srcs = ["doa.c", "sound_class.c", "siren_focus.c", "target_output.c", "dir_filter.c"]
    subprocess.run([g.GCC, "-O2", "-std=gnu99", "-Wall", "-DSC_TARGET_ONLY=1", f"-DDOA_FOCUS_ENABLE={focus}", *os.environ.get("INTERF_CFLAGS", "").split(),
                    "-I", SRC] + [os.path.join(SRC, s) for s in srcs] +
                   [os.path.join(HERE, "test_target_doa.c"), "-o", exe, "-lm"], check=True, env=env)
    return exe, env


def run(exe, env, data, cal=None):
    """cal = (块号, 距离米)：在该块模拟串口发 'C' 做距离标定"""
    path = os.path.abspath(os.path.join(g.BUILD, "interf.bin"))
    with open(path, "wb") as fp:
        fp.write(data)
    args = [exe, path, str(g.NFR)] + ([str(cal[0]), str(cal[1])] if cal else [])
    out = subprocess.run(args, capture_output=True, text=True, check=True, env=env).stdout
    rows = []
    for line in out.splitlines():
        v = line.split()
        rows.append(dict(active=int(v[1]), dvalid=int(v[2]), az=float(v[3]), raw=float(v[4]),
                         blk_az=float(v[5]), locked=int(v[6]), track=float(v[7]), band=int(v[8]),
                         foc_az=float(v[9]), foc_pr=float(v[10]), level=float(v[11]), dist=float(v[12]),
                         cal=int(v[13])))
    return rows


def knocks(t_list, amp):
    out = []
    for t in t_list:
        out.append((t, g.clap_sig(amp), 30.0))
    return out


def scenario(freq_fn=g.wail):
    A = 0.05
    dur = 20.0
    siren = g.tone_sig(freq_fn, 17.0, A)                      # 警笛 1.5~18.5s，120°
    srcs = [(1.5, siren, SIREN_AZ),
            (5.0, g.speech_sig(4.0, 4.0 * A), 300.0),          # 更响的说话（+12dB），300°
            (10.0, g.music_sig(3.0, 3.0 * A), 210.0)]          # 更响的拨弦音乐，210°
    srcs += knocks(np.arange(13.5, 16.0, 0.5), 2.0 * A)       # 连续敲击，30°
    x = g.timeline_render(srcs, dur, amb_level=A * 10 ** (-38 / 20))
    data, nb = g.timeline_packets(x)
    return data


def scenario_pre(freq_fn=g.wail):
    """干扰先出现：更响的音乐 210° 从 0.5s 开始一直响到 9s，警笛 2.5s 才出现（120°），之后说话 300°"""
    A = 0.05
    dur = 16.0
    srcs = [(0.5, g.music_sig(8.5, 3.0 * A), 210.0),
            (2.5, g.tone_sig(freq_fn, 13.0, A), SIREN_AZ),
            (10.0, g.speech_sig(4.0, 4.0 * A), 300.0)]
    x = g.timeline_render(srcs, dur, amb_level=A * 10 ** (-38 / 20))
    data, nb = g.timeline_packets(x)
    return data


def scenario_nosiren():
    """没有警笛：音乐、说话、敲击、持续单音口哨（1200Hz），都不应报警笛"""
    A = 0.05
    dur = 20.0
    whistle = g.tone_sig(lambda t: np.full_like(t, 1200.0), 3.0, 2.0 * A, harmonics=2)
    srcs = [(0.5, g.music_sig(6.0, 3.0 * A), 210.0),
            (7.0, g.speech_sig(5.0, 4.0 * A), 300.0),
            (12.5, whistle, 60.0),
            (16.0, g.music_sig(3.5, 3.0 * A), 150.0)]
    srcs += knocks(np.arange(9.0, 11.0, 0.5), 2.0 * A)
    x = g.timeline_render(srcs, dur, amb_level=A * 10 ** (-38 / 20))
    return g.timeline_packets(x)[0]


def scenario_alone(az, freq_fn):
    A = 0.05
    x = g.timeline_render([(0.5, g.tone_sig(freq_fn, 8.0, A), az)], 9.0, amb_level=A * 10 ** (-38 / 20))
    return g.timeline_packets(x)[0]


def scenario_moving(freq_fn):
    """警笛连续发声：0.5~6s 在 60°，6~11s 换到 200°（突变）；11~17s 从 200° 匀速转到 290°（每 0.5s 走 7.5°）"""
    A = 0.05
    sig = g.tone_sig(freq_fn, 16.5, A)
    cuts = [(0.5, 6.0, 60.0), (6.0, 11.0, 200.0)] + \
           [(11.0 + 0.5 * k, 11.5 + 0.5 * k, 200.0 + 7.5 * (k + 1)) for k in range(12)]
    srcs = []
    for t0, t1, az in cuts:
        i0, i1 = int((t0 - 0.5) * g.FS), min(int((t1 - 0.5) * g.FS), len(sig))
        srcs.append((t0, sig[i0:i1], az))
    x = g.timeline_render(srcs, 17.5, amb_level=A * 10 ** (-38 / 20))
    return g.timeline_packets(x)[0]


def moving_check(rows):
    """返回（换位后多久显示到新方向 ±15° 内，匀速转动段显示误差中位数）"""
    bt = g.NFR / g.FS
    follow = None
    for i in range(int(6.0 / bt), int(11.0 / bt)):
        r = rows[i]
        if r["active"] and r["dvalid"] and abs(g.angerr(r["az"], 200.0)) <= 15.0:
            follow = i * bt - 6.0
            break
    errs = []
    for i in range(int(12.0 / bt), int(17.0 / bt)):
        r = rows[i]
        if r["active"] and r["dvalid"]:
            truth = 200.0 + 7.5 * (int((i * bt - 11.0) / 0.5) + 1)
            errs.append(abs(g.angerr(r["az"], truth)))
    return follow, (np.median(errs) if errs else 999.0)


def band_noise(dur, rms, seed):
    from scipy.signal import butter, sosfilt
    r = np.random.default_rng(seed)
    x = sosfilt(butter(4, [150, 4000], "bandpass", fs=g.FS, output="sos"), r.standard_normal(int(dur * g.FS)))
    return x / np.std(x) * rms


def scenario_noise(freq_fn, kind, rel_db):
    """持续宽带噪声（150~4000Hz，总功率比警笛高 rel_db）：dir = 300° 方向的噪声源（风扇、喇叭放白噪声），
    diffuse = 各麦克风独立的散射噪声（人声嘈杂、车流）。警笛 120°，1~11s"""
    A = 0.05
    siren = g.tone_sig(freq_fn, 10.0, A)
    nrms = np.std(siren[int(g.FS):-int(g.FS)]) * 10 ** (rel_db / 20)
    srcs = [(1.0, siren, SIREN_AZ)]
    if kind == "dir":
        srcs.append((0.5, band_noise(10.5, nrms, 11), 300.0))
    x = g.timeline_render(srcs, 11.5, amb_level=A * 10 ** (-38 / 20)).astype(float)
    if kind == "diffuse":
        d = np.stack([band_noise(11.5, nrms, 20 + c) for c in range(8)]) * 2 ** 23
        d[6] = 0
        x = x + d
    return g.timeline_packets(np.clip(np.round(x), -2 ** 23, 2 ** 23 - 1).astype(np.int64))[0]


def scenario_traffic():
    """合成交通噪声 30s：路面轰鸣（60~1500Hz）+ 5 辆车驶过（发动机谐波，基频 40~160Hz 随加减速滑动，音量先大后小）
    + 汽车喇叭（420/510Hz 两音同时响，含 2.5s 长鸣）。不应报警笛"""
    from scipy.signal import butter, sosfilt
    r = np.random.default_rng(7)
    A = 0.05

    def engine(dur, f0a, f0b, amp):
        t = np.arange(int(dur * g.FS)) / g.FS
        f0 = f0a + (f0b - f0a) * (0.5 - 0.5 * np.cos(np.pi * t / dur))
        ph = 2 * np.pi * np.cumsum(f0) / g.FS
        x = sum(np.sin(h * ph + r.uniform(0, 6.28)) / h ** 0.7 for h in range(1, 30))
        return amp * x * np.exp(-((t - dur / 2) / (dur / 3)) ** 2) / 3

    def horn(dur, amp):
        t = np.arange(int(dur * g.FS)) / g.FS
        x = sum(np.sin(2 * np.pi * h * f * t) / h for f in (420, 510) for h in range(1, 6))
        return amp * x * np.minimum(1, np.minimum(t, dur - t) / 0.02)

    rum = sosfilt(butter(2, [60, 1500], "bandpass", fs=g.FS, output="sos"), r.standard_normal(int(30 * g.FS)))
    srcs = [(0.0, A * 0.5 * rum / np.std(rum), 200.0)]
    for t0, d, a, b, az in [(2, 6, 60, 140, 40), (7, 5, 120, 70, 300), (12, 8, 40, 110, 150),
                            (19, 6, 90, 160, 250), (24, 5, 150, 80, 80)]:
        srcs.append((t0, engine(d, a, b, A * 2), az))
    for t0, d in [(5, 0.6), (9, 1.5), (15, 0.4), (16, 0.4), (22, 2.5)]:
        srcs.append((t0, horn(d, A), 330.0))
    return g.timeline_packets(g.timeline_render(srcs, 30.0, amb_level=A * 10 ** (-38 / 20)))[0]


DIST_STEPS = [(0.5, 6.5, 1.0), (6.5, 12.5, 2.0), (12.5, 18.5, 4.0), (18.5, 24.5, 0.6)]


def scenario_distance(freq_fn):
    """警笛 90° 连续发声，音量按距离反比变化：1m（在 5.5s 标定）→ 2m → 4m → 0.6m"""
    A = 0.05
    sig = g.tone_sig(freq_fn, 24.0, A)
    srcs = []
    for t0, t1, d in DIST_STEPS:
        i0, i1 = int((t0 - 0.5) * g.FS), int((t1 - 0.5) * g.FS)
        srcs.append((t0, sig[i0:i1] / d, 90.0))
    return g.timeline_packets(g.timeline_render(srcs, 25.0, amb_level=A * 10 ** (-38 / 20)))[0]


SEGS_MAIN = (3.5, 18.0, [("说话干扰段", 5.0, 9.0), ("音乐干扰段", 10.0, 13.0), ("敲击干扰段", 13.5, 16.0)])
SEGS_PRE = (5.0, 15.0, [("音乐(先于警笛)段", 5.0, 9.0), ("说话干扰段", 10.0, 14.0)])


def evaluate(rows, name, segs=SEGS_MAIN):
    bt = g.NFR / g.FS
    seg = rows[int(segs[0] / bt):int(segs[1] / bt)]            # 警笛已确认后的持续段
    act = np.mean([r["active"] for r in seg])
    shown = [r for r in seg if r["active"] and r["dvalid"]]
    err = np.array([abs(g.angerr(r["az"], SIREN_AZ)) for r in shown]) if shown else np.array([999.0])
    wrong = np.mean(err > 30.0) if shown else 1.0
    cover = len(shown) / max(len(seg), 1)
    print(f"  {name}: 警笛检出 {act * 100:.0f}%，显示方向的块 {cover * 100:.0f}%，"
          f"方向误差中位数 {np.median(err):.1f}°，指向错误(>30°) {wrong * 100:.1f}%")
    for nm, t0, t1 in segs[2]:
        s2 = [r for r in rows[int(t0 / bt):int(t1 / bt)] if r["active"] and r["dvalid"]]
        e2 = [abs(g.angerr(r["az"], SIREN_AZ)) for r in s2]
        raw = [abs(g.angerr(r["raw"], SIREN_AZ)) for r in rows[int(t0 / bt):int(t1 / bt)]]
        print(f"      {nm}: 显示方向误差中位数 {np.median(e2) if e2 else float('nan'):.1f}°"
              f"（显示 {len(s2)} 块），宽带原始方位误差中位数 {np.median(raw):.1f}°")
    return act, wrong, cover


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    os.makedirs(g.BUILD, exist_ok=True)
    ok = True
    exes = {focus: build(focus) for focus in (0, 1)}
    cases = []
    for sname, fn in [("升降调", g.wail), ("两音", g.two_tone)]:
        cases.append((f"{sname}警笛 120°，更响的说话 300°（+12dB）、音乐 210°、敲击 30°", scenario(fn), SEGS_MAIN))
        cases.append((f"{sname}警笛 120°，更响的音乐 210° 先于警笛出现，之后说话 300°", scenario_pre(fn), SEGS_PRE))
    for title, data, segs in cases:
        print(f"抗干扰回归（{title}）：")
        res = {}
        for focus in (0, 1):
            exe, env = exes[focus]
            res[focus] = evaluate(run(exe, env, data), "聚焦定位 " + ("开" if focus else "关"), segs)
        act, wrong, cover = res[1]
        ok = ok and act >= 0.8 and wrong <= 0.02 and cover >= 0.5

    exe, env = exes[1]
    bt = g.NFR / g.FS
    print("持续宽带噪声（警笛 120°）：")
    for kind, rel in [("dir", 0), ("dir", 6), ("diffuse", 0), ("diffuse", 6)]:
        for fn in (g.wail, g.two_tone):
            rows = run(exe, env, scenario_noise(fn, kind, rel))[int(3.5 / bt):int(10.5 / bt)]
            act = np.mean([r["active"] for r in rows])
            shown = [r for r in rows if r["active"] and r["dvalid"]]
            err = np.array([abs(g.angerr(r["az"], SIREN_AZ)) for r in shown]) if shown else np.array([0.0])
            wrong = np.mean(err > 30.0) if shown else 0.0
            cover = len(shown) / len(rows)
            name = ("方向噪声 300°" if kind == "dir" else "散射噪声") + f" +{rel}dB {fn.__name__}"
            print(f"  {name}: 检出 {act * 100:.0f}%，显示方向 {cover * 100:.0f}%，"
                  f"误差中位数 {np.median(err):.1f}°，指向错误 {wrong * 100:.1f}%")
            # 噪声更响时允许只显示"未知"，但绝不能指错
            ok = ok and act >= 0.8 and wrong <= 0.02 and (cover >= 0.5 or (rel > 0 and fn is g.two_tone))

    # 实录的安静房间/背景（含持续的嗡嗡声）：不能误报警笛
    import glob
    rec = os.path.join(HERE, "..", "..", "rec")
    for f in sorted(glob.glob(os.path.join(rec, "room_now_*.npz")) + glob.glob(os.path.join(rec, "false_alarm_*.npz")) +
                    glob.glob(os.path.join(rec, "dataset", "background", "*", "raw.npz"))):
        with np.load(f) as d:
            smp = d["samples"]
        rows = run(exe, env, b"".join(g.pack(smp[i].T, i * g.NFR) for i in range(len(smp))))
        fa = np.mean([r["active"] for r in rows])
        print(f"实录背景 {os.path.basename(os.path.dirname(f)) if f.endswith('raw.npz') else os.path.basename(f)}："
              f"误报警笛的块 {fa * 100:.1f}%")
        ok = ok and fa <= 0.01

    # 按警笛音量估距离：1 米处标定后，2m / 4m / 0.6m 的估计距离与档位
    for fn in (g.wail, g.two_tone):
        rows = run(exe, env, scenario_distance(fn), cal=(int(5.5 / bt), 1.0))
        cal_ok = any(r["cal"] == 1 for r in rows)
        res = []
        for t0, t1, d in DIST_STEPS[1:]:
            seg = [r for r in rows[int((t0 + 2.0) / bt):int(t1 / bt)] if r["active"] and r["dvalid"] and r["dist"] > 0]
            est = np.median([r["dist"] for r in seg]) if seg else -1.0
            band = np.bincount([r["band"] for r in seg], minlength=4).argmax() if seg else 0
            want = 1 if d < 1.5 else 3 if d > 4.0 else 2
            want = 3 if d >= 4.0 else want          # 4m 处于中/远边界，接受远或中
            res.append((d, est, band))
            ok = ok and est > 0 and abs(np.log(est / d)) <= np.log(1.3) and (band == want or (d >= 4.0 and band in (2, 3)))
        ok = ok and cal_ok
        print(f"距离估计（{fn.__name__}，1m 处标定{'成功' if cal_ok else '失败'}）：" +
              "，".join(f"{d}m→估计 {e:.2f}m 档位{'?近中远'[b]}" for d, e, b in res))

    # 实录救护车（标注约 1 米）：用默认参考音量应显示"近"
    amb = sorted(glob.glob(os.path.join(rec, "dataset", "ambulance", "*T141016*", "raw.npz")))
    bgf = sorted(glob.glob(os.path.join(rec, "dataset", "background", "*T140447*", "raw.npz")))
    if amb and bgf:
        with np.load(bgf[0]) as d:
            smp = d["samples"][-96:]
        with np.load(amb[0]) as d:
            smp = np.concatenate((smp, d["samples"]))
        rows = run(exe, env, b"".join(g.pack(smp[i].T, i * g.NFR) for i in range(len(smp))))[96:]
        shown = [r for r in rows if r["active"] and r["dvalid"]]
        near = np.mean([r["band"] == 1 for r in shown]) if shown else 0.0
        dist = np.median([r["dist"] for r in shown if r["dist"] > 0]) if shown else -1
        print(f"实录救护车（约 1 米）：显示方向 {len(shown)} 块，其中近距离 {near * 100:.0f}%，估计距离中位数 {dist:.2f}m")
        ok = ok and near >= 0.8

    rows = run(exe, env, scenario_traffic())
    fa = np.mean([r["active"] for r in rows])
    print(f"合成交通噪声（发动机、喇叭长鸣、路面轰鸣）：误报警笛的块 {fa * 100:.1f}%")
    ok = ok and fa <= 0.01

    rows = run(exe, env, scenario_nosiren())
    fa = np.mean([r["active"] for r in rows])
    print(f"无警笛（音乐、说话、敲击、1200Hz 口哨）：误报警笛的块 {fa * 100:.1f}%")
    ok = ok and fa <= 0.02

    bt = g.NFR / g.FS
    errs = []
    for az in (0.0, 45.0, 100.0, 180.0, 250.0, 330.0):
        for fn in (g.wail, g.two_tone):
            rows = run(exe, env, scenario_alone(az, fn))[int(2.5 / bt):int(8.5 / bt)]
            shown = [r for r in rows if r["active"] and r["dvalid"]]
            e = np.median([abs(g.angerr(r["az"], az)) for r in shown]) if shown else 999.0
            errs.append((az, fn.__name__, len(shown) / len(rows), e))
    print("单独警笛：" + "，".join(f"{a:.0f}°{n} 显示{c * 100:.0f}% 误差{e:.1f}°" for a, n, c, e in errs))
    ok = ok and all(c >= 0.8 and e <= 10.0 for _, _, c, e in errs)

    for fn in (g.wail, g.two_tone):
        follow, merr = moving_check(run(exe, env, scenario_moving(fn)))
        fs = f"{follow:.2f}s" if follow is not None else "未跟上"
        print(f"移动警笛（{fn.__name__}）：60°→200° 换位后 {fs} 显示到新方向；200°→290° 匀速转动段显示误差中位数 {merr:.1f}°")
        ok = ok and follow is not None and follow <= 1.5 and merr <= 15.0
    print("抗干扰回归：" + ("通过" if ok else "未通过"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
