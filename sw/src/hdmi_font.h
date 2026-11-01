/* 由 tools/gen_cjk_font.py 生成，请勿手改 */
#ifndef HDMI_FONT_H
#define HDMI_FONT_H

#include <stdint.h>

typedef struct {
    uint16_t cp;          /* Unicode 码位 */
    uint8_t  w, h;        /* 位图尺寸 */
    int8_t   xo, yo;      /* 位图左上角相对 (笔位置, 基线) 的偏移 */
    uint8_t  adv;         /* 前进宽度 */
    uint32_t off;         /* 在 bits 中的字节偏移；每行 (w+3)/4 字节，每像素 2bit，低位在左 */
} hfont_glyph_t;

typedef struct {
    const hfont_glyph_t *g;   /* 按 cp 升序 */
    int n;
    const uint8_t *bits;
    int ascent, height;
} hfont_t;

extern const hfont_t hfont_s;      /* 16px，174 字 */
extern const hfont_t hfont_m;      /* 22px，174 字 */
extern const hfont_t hfont_l;      /* 34px，91 字 */
extern const hfont_t hfont_xl;      /* 80px，12 字 */

#endif
