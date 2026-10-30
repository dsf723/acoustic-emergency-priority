"""
编译 oled_ui.c 的 PC 测试程序，把渲染出的帧缓冲（SSD1306 页格式）转成放大的 PNG 预览图
用法：python preview_oled.py      输出 build/oled_preview.png
"""
import os
import subprocess

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
BUILD = os.path.join(HERE, "build")
GCC = os.environ.get("GCC", r"D:\Xilinx\Vivado\2018.3\tps\mingw\6.2.0\win64.o\nt\bin\gcc.exe")


def main():
    os.makedirs(BUILD, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(GCC) + os.pathsep + env["PATH"]
    exe = os.path.join(BUILD, "test_oled_ui.exe")
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-I", SRC, os.path.join(SRC, "oled_ui.c"),
                    os.path.join(HERE, "test_oled_ui.c"), "-o", exe, "-lm"], check=True, env=env)
    raw = os.path.join(BUILD, "fb.bin")
    subprocess.run([exe, raw], check=True, env=env)

    data = np.fromfile(raw, dtype=np.uint8).reshape(-1, 8, 128)
    scale, gap = 4, 8
    frames = []
    for fb in data:
        bits = np.unpackbits(fb[:, None, :], axis=1, bitorder="little")   # [8 页, 8 行, 128]
        img = bits.reshape(64, 128)
        rgb = np.zeros((64, 128, 3), np.uint8)
        rgb[img == 1] = (120, 200, 255)
        rgb[img == 0] = (10, 10, 20)
        frames.append(np.kron(rgb, np.ones((scale, scale, 1), np.uint8)))
    h, w = frames[0].shape[:2]
    nrows = (len(frames) + 1) // 2
    canvas = np.full((nrows * h + (nrows - 1) * gap, 2 * w + gap, 3), 255, np.uint8)
    for i, f in enumerate(frames):
        y, x = (i // 2) * (h + gap), (i % 2) * (w + gap)
        canvas[y:y + h, x:x + w] = f
    out = os.path.join(BUILD, "oled_preview.png")
    Image.fromarray(canvas).save(out)
    print("written", out)


if __name__ == "__main__":
    main()
