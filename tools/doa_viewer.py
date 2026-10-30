"""
PC 端声源方向显示：读取开发板串口输出，在极坐标图上实时显示声源方向。

用法：python doa_viewer.py COM5        （需要 pip install pyserial matplotlib）
串口格式见 sw/src/main.c：
    DOA,<角度x10>,<信噪比dBx10>,<峰值比x100>,<滤波后角度x10>
    EVT,<CLAP|SIREN>,<方位x10>,<NEAR|MID|FAR|UNKNOWN>   /   EVT,SIREN_END
    LVL,<ch0>,...,<ch7>,<耗时us>,<overrun>
"""
import sys
import time

import matplotlib.pyplot as plt
import numpy as np
import serial


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    ser = serial.Serial(sys.argv[1], 115200, timeout=0.05)

    plt.ion()
    fig = plt.figure("Sound source direction")
    ax = fig.add_subplot(111, projection="polar")
    ax.set_ylim(0, 1)
    ax.set_yticks([])
    (needle,) = ax.plot([0, 0], [0, 0.95], lw=4)
    hist = ax.scatter([], [], s=12, alpha=0.4)
    txt = fig.text(0.02, 0.02, "", family="monospace")
    pts = []
    last = None
    event = None
    buf = b""

    while plt.fignum_exists(fig.number):
        buf += ser.read(ser.in_waiting or 1)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode(errors="ignore").strip()
            if line.startswith("DOA,"):
                v = line.split(",")
                az = int(v[4]) / 10.0 if len(v) > 4 else int(v[1]) / 10.0   # 优先用滤波后角度
                last = (time.time(), az, int(v[2]) / 10.0, int(v[3]) / 100.0)
                pts.append((time.time(), np.deg2rad(az)))
            elif line.startswith("EVT,"):
                v = line.split(",")
                if v[1] == "SIREN_END":
                    event = None
                else:
                    event = (time.time(), v[1], v[3] if len(v) == 4 else v[4])
                print(line)
            elif line.startswith("RNG,"):
                v = line.split(",")
                if len(v) == 3:
                    event = None if v[1] == "NONE" else (time.time(), v[1], v[2])
            elif line.startswith("LVL,"):
                v = line.split(",")
                txt.set_text("ch dBFS: " + " ".join(v[1:9]) + f"   proc={v[9]}us overrun={v[10]}")
            elif line:
                print(line)

        now = time.time()
        pts = [p for p in pts if now - p[0] < 3.0]
        if pts:
            hist.set_offsets(np.c_[[p[1] for p in pts], np.full(len(pts), 0.85)])
        else:
            hist.set_offsets(np.empty((0, 2)))
        if last and now - last[0] < 1.0:
            th = np.deg2rad(last[1])
            needle.set_data([th, th], [0, 0.95])
            needle.set_alpha(1.0)
            ev = f"{event[1]} {event[2]}   " if event and now - event[0] < 3.0 else ""
            ax.set_title(f"{ev}{last[1]:6.1f} deg   SNR {last[2]:4.1f} dB")
        else:
            needle.set_alpha(0.15)
        plt.pause(0.03)


if __name__ == "__main__":
    main()
