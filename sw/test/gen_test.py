"""
声源定位算法 PC 端验证

按 doa_config.h 的默认几何（半径 4cm，0 号麦在 0°，逆时针每 60°，通道 0..5 + 中心 7）
合成远场声源到达各麦克风的信号（频域施加分数时延），加独立噪声，量化成 24bit，
打包成与 PL mic_array_i2s_rx 完全相同的 DMA 字格式，交给 test_doa.exe 处理，
统计各方位角、各信噪比下的估计误差；
再在一条连续的时间线上合成拍手（含房间混响）、两音/升降调警笛、口哨、语音、持续噪声，
检验声音识别（拍手 / 警笛）的检出、误报与远近等级的合法性（不计算米数）。

用法：python gen_test.py            （需要 numpy；自动用 Vivado 自带 MinGW 编译测试程序）
"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
BUILD = os.path.join(HERE, "build")
GCC = os.environ.get("GCC", r"D:\Xilinx\Vivado\2018.3\tps\mingw\6.2.0\win64.o\nt\bin\gcc.exe")

FS = 48828.125
C = 343.0
R = 0.040
NFR = 1024              # 每包帧数
CH_OUTER = [0, 1, 2, 3, 4, 5]
CH_CENTER = 7                   # 实测：中心麦在 ch7，ch6 为空声道

rng = np.random.default_rng(1)


def mic_positions():
    ang = [60 * k for k in range(6)]               # 与 doa_config.h 一致：MIC0=0°，DIR=+1
    pos = [(R * np.cos(np.deg2rad(a)), R * np.sin(np.deg2rad(a))) for a in ang]
    pos.append((0.0, 0.0))
    return np.array(pos), CH_OUTER + [CH_CENTER]


def source(kind, n):
    if kind == "noise":                     # 宽带噪声（类似拍手/摩擦声）
        x = rng.standard_normal(n)
    elif kind == "speech":                  # 带谐波、慢变包络的类语音信号
        t = np.arange(n) / FS
        f0 = 140 + 30 * np.sin(2 * np.pi * 3 * t)
        ph = 2 * np.pi * np.cumsum(f0) / FS
        x = sum(np.sin(h * ph) / h for h in range(1, 25))
        x *= 0.6 + 0.4 * np.sin(2 * np.pi * 5 * t)
        x += 0.05 * rng.standard_normal(n)
    else:
        raise ValueError(kind)
    return x / np.std(x)


def render(az_deg, snr_db, kind, n, level=0.05, echo=False, dist=None):
    """返回 [8, n] 的 int 采样（8 个 PL 通道）；dist 为声源距离（米），None 表示远场平面波"""
    pos, chans = mic_positions()
    u = np.array([np.cos(np.deg2rad(az_deg)), np.sin(np.deg2rad(az_deg))])
    src = None if dist is None else dist * u
    pad = 4096
    s = source(kind, n + pad)
    S = np.fft.rfft(s)
    w = 2 * np.pi * np.fft.rfftfreq(len(s), 1 / FS)
    out = np.zeros((8, n))
    noise_lvl = level * 10 ** (-snr_db / 20)
    for (p, ch) in zip(pos, chans):
        if src is None:
            t_m, amp = -(p @ u) / C, 1.0
        else:
            d_m = np.linalg.norm(src - p)
            t_m, amp = d_m / C, dist / d_m          # 球面波：时延 + 1/r 幅度
        X = amp * S * np.exp(-1j * w * t_m)
        if echo:                            # 一个来自墙面的反射：另一个方向、延迟 3ms、幅度 0.5
            u2 = np.array([np.cos(np.deg2rad(az_deg + 110)), np.sin(np.deg2rad(az_deg + 110))])
            X = X + 0.5 * S * np.exp(-1j * w * (-(p @ u2) / C + 0.003))
        x = np.fft.irfft(X, len(s))[pad // 2: pad // 2 + n]
        out[ch] = level * x + noise_lvl * rng.standard_normal(n)
    out[6] = 0.0                            # 中心麦所在数据线的空声道（实测为 ch6）
    return np.clip(np.round(out * 2 ** 23), -2 ** 23, 2 ** 23 - 1).astype(np.int64)


def pack(samples, seq0):
    """samples [8, NFR] -> DMA 字流，格式 {seq[3:0], 0, ch[2:0], sample[23:0]}"""
    seq = ((seq0 + np.arange(NFR)) & 15).astype(np.uint32)[:, None]
    ch = np.arange(8, dtype=np.uint32)[None, :]
    smp = (samples.T.astype(np.int64) & 0xFFFFFF).astype(np.uint32)
    return ((seq << 28) | (ch << 24) | smp).astype(np.uint32).tobytes()


def noise_pkt(seq0, level=0.05 * 10 ** (-30 / 20)):
    x = level * rng.standard_normal((8, NFR))
    return pack(np.round(x * 2 ** 23).astype(np.int64), seq0)


def build(target_only=False):
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "test_target_doa.exe" if target_only else "test_doa.exe")
    cmd = [GCC, "-O2", "-std=gnu99", "-Wall", f"-DSC_TARGET_ONLY={int(target_only)}", "-I", SRC,
           os.path.join(SRC, "doa.c"), os.path.join(SRC, "sound_class.c"),
           os.path.join(HERE, "test_doa.c"), "-o", exe, "-lm"]
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(GCC) + os.pathsep + env["PATH"]
    subprocess.run(cmd, check=True, env=env)
    return exe, env


def run(exe, env, data):
    path = os.path.join(BUILD, "pkts.bin")
    with open(path, "wb") as fp:
        fp.write(data)
    out = subprocess.run([exe, path, str(NFR)], capture_output=True, text=True, check=True, env=env)
    rows = []
    for line in out.stdout.split("\n"):
        if line.strip():
            v = line.split()
            rows.append(dict(ret=int(v[1]), valid=int(v[2]), az=float(v[3]), ratio=float(v[4]),
                             e=float(v[5]), noise=float(v[6]), coh=float(v[7]), flat=float(v[8]),
                             cent=float(v[9]), peak=float(v[10]), tonal=float(v[11]),
                             cls=int(v[12]), event=int(v[13]), siren=int(v[14]), dist=float(v[15]),
                             band=int(v[16]), dir_ok=int(v[17]), event_az=float(v[18]),
                             plevel=float(v[19]), crest=float(v[20])))
    return rows


SND_CLAP, SND_SIREN = 2, 3


def timeline_render(sources, dur, amb_level=0.05 * 10 ** (-30 / 20)):
    """sources: [(起始秒, 单声道信号, 方位)]，返回连续时间线的 [8, N] int 采样"""
    pos, chans = mic_positions()
    n = int(dur * FS)
    w = 2 * np.pi * np.fft.rfftfreq(n, 1 / FS)
    out = amb_level * rng.standard_normal((8, n))
    for t0, sig, az in sources:
        mono = np.zeros(n)
        i0 = int(t0 * FS)
        seg = sig[: n - i0]
        mono[i0:i0 + len(seg)] = seg
        S = np.fft.rfft(mono)
        u = np.array([np.cos(np.deg2rad(az)), np.sin(np.deg2rad(az))])
        for p, ch in zip(pos, chans):
            out[ch] += np.fft.irfft(S * np.exp(-1j * w * (-(p @ u) / C)), n)
    out[6] = 0.0
    return np.clip(np.round(out * 2 ** 23), -2 ** 23, 2 ** 23 - 1).astype(np.int64)


def timeline_packets(x):
    nb = x.shape[1] // NFR
    return b"".join(pack(x[:, i * NFR:(i + 1) * NFR], i * NFR) for i in range(nb)), nb


def clap_sig(amp):
    """拍手：约 3ms 衰减的宽带脉冲 + 约 0.4s 的弥散混响尾（-15dB）"""
    t = np.arange(int(0.6 * FS)) / FS
    direct = rng.standard_normal(len(t)) * np.exp(-t / 0.003)
    tail = 0.18 * rng.standard_normal(len(t)) * np.exp(-t / 0.06) * (t > 0.002)
    return amp * (direct + tail)


def tone_sig(freq_fn, dur, amp, harmonics=3):
    t = np.arange(int(dur * FS)) / FS
    ph = 2 * np.pi * np.cumsum(freq_fn(t)) / FS
    x = sum(np.sin(h * ph) / h for h in range(1, harmonics + 1))
    ramp = np.minimum(1, np.minimum(t, dur - t) / 0.02)
    return amp * x * ramp


def two_tone(t):
    return np.where((t % 1.0) < 0.5, 700.0, 1000.0)


def wail(t):
    return 1000 + 400 * np.sin(2 * np.pi * t / 4.0)


def speech_sig(dur, amp):
    t = np.arange(int(dur * FS)) / FS
    f0 = 140 + 40 * np.sin(2 * np.pi * 2.3 * t)
    ph = 2 * np.pi * np.cumsum(f0) / FS
    x = sum(np.sin(h * ph) / h for h in range(1, 25))
    syll = (np.sin(2 * np.pi * 4 * t) > 0.2).astype(float)          # 音节间停顿
    return amp * x * syll


def music_sig(dur, amp):
    """拨弦/钢琴类音乐：每 0.25s 一个音，起音陡、指数衰减、带谐波"""
    t = np.arange(int(dur * FS)) / FS
    x = np.zeros_like(t)
    for k in range(int(dur / 0.25)):
        f0 = 220 * 2 ** (rng.integers(0, 24) / 12)
        tt = t - k * 0.25
        env = np.where(tt >= 0, np.exp(-np.maximum(tt, 0) / 0.15), 0)
        x += env * sum(np.sin(2 * np.pi * h * f0 * tt) / h for h in range(1, 6))
    return amp * x


def classification_test(exe, env):
    ok = True
    A = 0.05
    # 拍手：不同方向与不同响度，仅验证识别，不把幅度变化解释为距离。
    AC = 0.3 * A                                # 与实测 1 米拍手同量级（短时峰值约 -37dBFS）
    claps = [(2.0, 30, AC), (3.5, 150, AC / 2), (5.0, 260, AC / 3), (6.5, 80, AC)]
    srcs = [(t, clap_sig(a), az) for t, az, a in claps]
    srcs += [(9.0, tone_sig(two_tone, 6.0, A), 120.0),               # 两音警笛 9~15s
             (18.0, tone_sig(wail, 8.0, A), 300.0),                  # 升降调警笛 18~26s
             (29.0, tone_sig(lambda t: 1500 + 0 * t, 3.0, A, 1), 45.0),   # 稳定口哨 29~32s
             (34.0, speech_sig(4.0, A), 200.0),                      # 语音 34~38s
             (40.0, A * rng.standard_normal(int(2.0 * FS)), 10.0),   # 持续噪声 40~42s
             (43.0, music_sig(4.0, 4 * A), 330.0)]                   # 拨弦音乐 43~47s
    x = timeline_render(srcs, 48.0, amb_level=A * 10 ** (-38 / 20))   # 背景与实测房间相当（约 -61dBFS）
    data, nb = timeline_packets(x)
    rows = run(exe, env, data)
    bt = NFR / FS

    def blk(t):
        return int(t / bt)

    # 1) 拍手检出、方向、远近接口
    clap_ev = [i for i, r in enumerate(rows) if r["event"] and r["cls"] == SND_CLAP]
    print("声音识别测试：")
    for t, az, a in claps:
        hit = [i for i in clap_ev if blk(t) - 1 <= i <= blk(t) + 9]
        if not hit:
            print(f"  拍手 t={t}s 方位 {az}°：未检出"); ok = False; continue
        r = rows[hit[0]]
        err = abs(angerr(r["event_az"], az)) if r["event_az"] >= 0 else 999
        print(f"  拍手 t={t}s 方位 {az}°：检出，方位误差 {err:.1f}°，远近档位 {r['band']}（0=未知）")
        if err > 10:
            ok = False
    # Legacy host replay column remains -1; no metre estimate is produced.
    if any(r['dist'] != -1 or r['band'] not in (0, 1, 2, 3) for r in rows):
        print("远近输出非法，或仍存在米数估计"); ok = False
    false_claps = [i for i in clap_ev if not any(blk(t) - 1 <= i <= blk(t) + 9 for t, _, _ in claps)]
    print(f"  拍手误报：{len(false_claps)} 次" + (f"（块 {false_claps}，约 {[round(i * bt, 1) for i in false_claps]}s）" if false_claps else ""))
    if false_claps:
        ok = False

    # 2) 警笛：持续期间（去掉开头 1.5s）检出率、方向
    for name, t0, t1, az in [("两音警笛", 9.0, 15.0, 120.0), ("升降调警笛", 18.0, 26.0, 300.0)]:
        seg = rows[blk(t0 + 1.5):blk(t1)]
        act = np.mean([r["siren"] for r in seg])
        azs = [r["az"] for r in seg if r["dir_ok"]]
        med_err = np.median([abs(angerr(a, az)) for a in azs]) if azs else 999
        print(f"  {name}：持续期检出率 {act * 100:.0f}%，方向可用块 {len(azs)}，方向误差中位数 {med_err:.1f}°，"
              f"远近档位 {seg[-1]['band']}（0=未知）")
        if act < 0.8 or med_err > 10:
            ok = False
        after = rows[blk(t1) + 96:blk(t1) + 100]
        if any(r["siren"] for r in after):
            print(f"  {name}：结束 2s 后仍判为警笛"); ok = False

    # 3) 非警笛声音不能判为警笛
    for name, t0, t1 in [("口哨", 29.0, 32.0), ("语音", 34.0, 38.0), ("持续噪声", 40.0, 42.0),
                         ("拨弦音乐", 43.0, 47.0)]:
        fa = np.mean([r["siren"] for r in rows[blk(t0):blk(t1)]])
        print(f"  {name}：误判为警笛 {fa * 100:.0f}%")
        if fa > 0:
            ok = False
    target_exe, target_env = build(target_only=True)
    target_rows = run(target_exe, target_env, data)
    target_ok = all(r['cls'] in (0, SND_SIREN) and (r['siren'] or not r['dir_ok'])
                    for r in target_rows)
    for t0, t1 in [(9.0, 15.0), (18.0, 26.0)]:
        active = np.mean([r['siren'] for r in target_rows[blk(t0 + 1.5):blk(t1)]])
        target_ok = target_ok and active >= 0.8
    for t0, t1 in [(0.0, 8.0), (29.0, 32.0), (34.0, 38.0), (40.0, 42.0), (43.0, 47.0)]:
        target_ok = target_ok and not any(r['siren'] for r in target_rows[blk(t0):blk(t1)])
    print('目标模式音频回归：' + ('通过（仅警笛候选输出；非救护车区分验证）' if target_ok else '失败'))
    ok = ok and target_ok
    return ok


def angerr(a, b):
    return (a - b + 180) % 360 - 180


def strong_third_harmonic_test():
    # Reproduce the observed 1845/2373Hz dominance of a 615/791Hz siren.
    t = np.arange(int(6 * FS)) / FS
    f0 = np.where(t % 0.9 < 0.45, 615.0, 791.0)
    phase = 2 * np.pi * np.cumsum(f0) / FS
    sound = 0.04 * (0.08 * np.sin(phase) + np.sin(3 * phase))
    x = timeline_render([(2.0, sound, 0.0)], 10.5, amb_level=0.0001)
    data, _ = timeline_packets(x)
    exe, env = build(target_only=True)
    rows = run(exe, env, data)
    start, end = int(3.5 * FS / NFR), int(8 * FS / NFR)
    fraction = np.mean([r['siren'] for r in rows[start:end]])
    stopped = not any(r['siren'] for r in rows[int(10 * FS / NFR):])
    print(f'强三次谐波警笛回归：持续段活动比例 {fraction:.0%}，结束退出={stopped}（合成用例）')
    return fraction >= 0.8 and stopped


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    exe, env = build()
    range_exe = os.path.join(BUILD, "test_range.exe")
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-DSC_TARGET_ONLY=0", "-I", SRC,
                    os.path.join(SRC, "sound_class.c"), os.path.join(HERE, "test_range.c"),
                    "-o", range_exe, "-lm"], check=True, env=env)
    subprocess.run([range_exe], check=True, env=env)
    guard_exe = os.path.join(BUILD, "test_direction_guard.exe")
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-I", SRC,
                    os.path.join(SRC, "dir_filter.c"), os.path.join(HERE, "test_direction_guard.c"),
                    "-o", guard_exe, "-lm"], check=True, env=env)
    subprocess.run([guard_exe], check=True, env=env)
    target_exe = os.path.join(BUILD, "test_target_mode.exe")
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-I", SRC,
                    os.path.join(SRC, "sound_class.c"), os.path.join(SRC, "target_output.c"),
                    os.path.join(SRC, "dir_filter.c"), os.path.join(HERE, "test_target_mode.c"),
                    "-o", target_exe, "-lm"], check=True, env=env)
    subprocess.run([target_exe], check=True, env=env)
    traffic_exe = os.path.join(BUILD, "test_traffic.exe")
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-I", SRC,
                    os.path.join(SRC, "traffic_light.c"), os.path.join(HERE, "test_traffic.c"),
                    "-o", traffic_exe, "-lm"], check=True, env=env)
    subprocess.run([traffic_exe], check=True, env=env)
    ok = True

    # 场景：先 10 包背景噪声建立噪底，再每个方位角 1 包有声源
    for kind, snr, echo in [("noise", 20, False), ("speech", 20, False),
                            ("noise", 5, False), ("speech", 10, True)]:
        azs = np.arange(0, 360, 5) + 0.7
        data = b"".join(noise_pkt(i * 4) for i in range(10))
        for i, az in enumerate(azs):
            data += pack(render(az, snr, kind, NFR), i)
        rows = run(exe, env, data)
        noise_rows, src_rows = rows[:10], rows[10:]
        errs = np.array([angerr(r["az"], az) for r, az in zip(src_rows, azs)])
        valid = np.array([r["valid"] for r in src_rows])
        ratios = np.array([r["ratio"] for r in src_rows])
        false_det = sum(r["valid"] for r in noise_rows[3:])
        print(f"{kind:6s} SNR={snr:2d}dB echo={int(echo)}: "
              f"RMS误差={np.sqrt(np.mean(errs ** 2)):.2f}° 最大={np.max(np.abs(errs)):.2f}° "
              f"检出率={valid.mean() * 100:.0f}% peak_ratio[min/med]={ratios.min():.2f}/{np.median(ratios):.2f} "
              f"噪声误检={false_det}")
        max_ok = 8 if echo else 5                      # 带强反射的场景允许稍大误差
        if np.max(np.abs(errs)) > max_ok or valid.mean() < 0.95 or false_det:
            ok = False

    if not classification_test(exe, env):
        ok = False
    if not strong_third_harmonic_test():
        ok = False

    # 板上同一链路（含 siren_focus）在更响的干扰下方向不被带走、无警笛时不误报
    sys.path.insert(0, HERE)
    import interference_test
    if interference_test.main() != 0:
        ok = False

    # 包内帧序号错误必须被识别
    bad = bytearray(pack(render(30, 20, "noise", NFR), 0))
    bad[100 * 32 + 3] ^= 0x30                   # 改第 100 帧 ch0 的 seq
    rows = run(exe, env, bytes(bad))
    if rows[0]["ret"] != -1:
        print("序号错误未被识别"); ok = False
    else:
        print("帧序号错误检测：正常")

    print("ALL PASSED" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
