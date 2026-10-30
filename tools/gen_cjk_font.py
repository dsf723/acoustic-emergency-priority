"""
生成 HDMI 界面用的抗锯齿点阵字库 sw/src/hdmi_font.c / hdmi_font.h

字体：Windows 自带的 Noto Sans SC（思源黑体同源，SIL OFL 开源授权，可嵌入）。
每像素 2bit 覆盖度（0~3），由 hdmi_ui.c 映射到调色板中相近的颜色做抗锯齿。
汉字集合自动从 hdmi_ui.c、main.c、oled_tl.c、traffic_light.c 的字符串常量中收集，界面改字后重新运行本脚本即可。
用法：python gen_cjk_font.py
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "sw", "src")
FONT = r"C:\Windows\Fonts\NotoSansSC-VF.ttf"

ASCII = "".join(chr(c) for c in range(32, 127))
DIGITS = "0123456789-°"


def ui_chars():
    chars = set()
    for name in ("hdmi_ui.c", "main.c", "oled_tl.c", "traffic_light.c"):
        with open(os.path.join(SRC, name), encoding="utf-8") as fp:
            for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', fp.read()):
                chars.update(ch for ch in lit if ord(ch) > 0x7F)
    return "".join(sorted(chars))


def render_font(name, px, weight, charset):
    font = ImageFont.truetype(FONT, px)
    font.set_variation_by_name(weight)
    ascent, descent = font.getmetrics()
    glyphs, bits = [], bytearray()
    for ch in sorted(set(charset), key=ord):
        adv = int(round(font.getlength(ch)))
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = max(x1 - x0, 0), max(y1 - y0, 0)
        off = len(bits)
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            px_ = img.load()
            stride = (w + 3) // 4
            for y in range(h):
                row = bytearray(stride)
                for x in range(w):
                    v = px_[x, y]
                    q = 0 if v < 40 else 1 if v < 110 else 2 if v < 190 else 3
                    row[x >> 2] |= q << ((x & 3) * 2)
                bits += row
        glyphs.append((ord(ch), w, h, x0, y0, adv, off))
    return dict(name=name, px=px, ascent=ascent, height=ascent + descent, glyphs=glyphs, bits=bits)


def emit(fonts):
    h = ["/* 由 tools/gen_cjk_font.py 生成，请勿手改 */",
         "#ifndef HDMI_FONT_H", "#define HDMI_FONT_H", "", "#include <stdint.h>", "",
         "typedef struct {",
         "    uint16_t cp;          /* Unicode 码位 */",
         "    uint8_t  w, h;        /* 位图尺寸 */",
         "    int8_t   xo, yo;      /* 位图左上角相对 (笔位置, 基线) 的偏移 */",
         "    uint8_t  adv;         /* 前进宽度 */",
         "    uint32_t off;         /* 在 bits 中的字节偏移；每行 (w+3)/4 字节，每像素 2bit，低位在左 */",
         "} hfont_glyph_t;", "",
         "typedef struct {",
         "    const hfont_glyph_t *g;   /* 按 cp 升序 */",
         "    int n;",
         "    const uint8_t *bits;",
         "    int ascent, height;",
         "} hfont_t;", ""]
    c = ["/* 由 tools/gen_cjk_font.py 生成，请勿手改。字形来自 Noto Sans SC（SIL Open Font License 1.1） */",
         '#include "hdmi_font.h"', ""]
    for f in fonts:
        n = f["name"]
        h.append(f"extern const hfont_t {n};      /* {f['px']}px，{len(f['glyphs'])} 字 */")
        c.append(f"static const uint8_t {n}_bits[{max(len(f['bits']), 1)}] = {{")
        b = f["bits"] or b"\0"
        for i in range(0, len(b), 24):
            c.append("    " + ",".join(f"0x{v:02x}" for v in b[i:i + 24]) + ",")
        c.append("};")
        c.append(f"static const hfont_glyph_t {n}_glyphs[{len(f['glyphs'])}] = {{")
        for cp, w, hh, xo, yo, adv, off in f["glyphs"]:
            c.append(f"    {{0x{cp:04x}, {w}, {hh}, {xo}, {yo}, {adv}, {off}}},")
        c.append("};")
        c.append(f"const hfont_t {n} = {{ {n}_glyphs, {len(f['glyphs'])}, {n}_bits, "
                 f"{f['ascent']}, {f['height']} }};")
        c.append("")
    h += ["", "#endif", ""]
    with open(os.path.join(SRC, "hdmi_font.h"), "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(h))
    with open(os.path.join(SRC, "hdmi_font.c"), "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(c))


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    cjk = ui_chars()
    fonts = [render_font("hfont_s", 16, "Medium", ASCII + cjk),
             render_font("hfont_m", 22, "Bold", ASCII + cjk),
             render_font("hfont_l", 34, "Bold", DIGITS + "/" + cjk),
             render_font("hfont_xl", 80, "Bold", DIGITS)]
    emit(fonts)
    total = sum(len(f["bits"]) for f in fonts)
    print(f"汉字 {len(cjk)} 个：{cjk}")
    print(f"字库位图共 {total / 1024:.1f} KB -> sw/src/hdmi_font.c")


if __name__ == "__main__":
    main()
