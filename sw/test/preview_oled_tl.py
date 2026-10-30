"""
编译 oled_tl.c 的 PC 测试程序，把信号灯倒计时画面（SSD1306 页格式）转成放大的 PNG 预览图
用法：python preview_oled_tl.py      输出 build/oled_tl_preview.png 与 docs/oled_tl_preview.png
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
    exe = os.path.join(BUILD, "test_oled_tl.exe")
    srcs = ["oled_tl.c", "oled_ui.c", "traffic_light.c", "hdmi_ui.c", "hdmi_font.c"]
    subprocess.run([GCC, "-O2", "-std=gnu99", "-Wall", "-I", SRC] + [os.path.join(SRC, s) for s in srcs] +
                   [os.path.join(HERE, "test_oled_tl.c"), "-o", exe, "-lm"], check=True, env=env)
    raw = os.path.join(BUILD, "oled_tl_fb.bin")
    subprocess.run([exe, raw], check=True, env=env)

    data = np.fromfile(raw, dtype=np.uint8).reshape(-1, 8, 128)
    scale, gap = 4, 8
    frames = []
    for fb in data:
        bits = np.unpackbits(fb[:, None, :], axis=1, bitorder="little").reshape(64, 128)
        img = np.where(bits[:, :, None] > 0, [120, 220, 255], [10, 14, 26]).astype(np.uint8)
        frames.append(np.kron(img, np.ones((scale, scale, 1), np.uint8)))
    h, w = frames[0].shape[:2]
    canvas = np.full((2 * h + gap, 2 * w + gap, 3), 255, np.uint8)
    for i, f in enumerate(frames[:4]):
        y, x = (i // 2) * (h + gap), (i % 2) * (w + gap)
        canvas[y:y + h, x:x + w] = f
    for out in (os.path.join(BUILD, "oled_tl_preview.png"), os.path.join(HERE, "..", "..", "docs", "oled_tl_preview.png")):
        Image.fromarray(canvas).save(out)
    print("written", os.path.normpath(out))


if __name__ == "__main__":
    main()
