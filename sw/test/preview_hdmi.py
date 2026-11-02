"""
编译 hdmi_ui.c 的 PC 测试程序，把渲染出的 4bit 帧缓冲按 PL 调色板转成 PNG 预览图
用法：python preview_hdmi.py      输出 build/hdmi_preview.png（4 种状态拼成 2x2）
"""
import os
import subprocess

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
BUILD = os.path.join(HERE, "build")
GCC = os.environ.get("GCC", r"D:\Xilinx\Vivado\2018.3\tps\mingw\6.2.0\win64.o\nt\bin\gcc.exe")

# 与 rtl/hdmi_fb.v 中的调色板一致
PALETTE = [0x0A0E1A, 0xFFFFFF, 0x3A4256, 0x8A93A8, 0xFF3030, 0x30E060, 0x3080FF, 0xFFD030,
           0x30E0E0, 0xE040E0, 0xFF8C20, 0x142040, 0x104020, 0x501010, 0xC8CED8, 0x000000]


def main():
    os.makedirs(BUILD, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(GCC) + os.pathsep + env["PATH"]
    exe = os.path.abspath(os.path.join(BUILD, "test_hdmi_ui.exe"))
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-I", SRC, os.path.join(SRC, "hdmi_ui.c"), os.path.join(SRC, "hdmi_font.c"), os.path.join(SRC, "traffic_light.c"),
                    os.path.join(HERE, "test_hdmi_ui.c"), "-o", exe, "-lm"], check=True, env=env)
    raw = os.path.abspath(os.path.join(BUILD, "hdmi_fb.bin"))
    subprocess.run([exe, raw], check=True, env=env)

    words = np.fromfile(raw, dtype="<u4").reshape(-1, 480, 80)
    pal = np.array([[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in PALETTE], np.uint8)
    frames = []
    for fb in words:
        idx = np.stack([(fb >> (4 * k)) & 15 for k in range(8)], axis=-1).reshape(480, 640)
        frames.append(pal[idx])
    gap = 12
    canvas = np.full((2 * 480 + gap, 2 * 640 + gap, 3), 255, np.uint8)
    for i, f in enumerate(frames[:4]):
        y, x = (i // 2) * (480 + gap), (i % 2) * (640 + gap)
        canvas[y:y + 480, x:x + 640] = f
    out = os.path.join(BUILD, "hdmi_preview.png")
    Image.fromarray(canvas).save(out)
    docs = os.path.join(HERE, "..", "..", "docs", "hdmi_preview.png")
    Image.fromarray(canvas).save(docs)
    print("written", out, "and", os.path.normpath(docs))


if __name__ == "__main__":
    main()
