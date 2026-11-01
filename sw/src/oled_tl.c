#include "oled_tl.h"

/* 竖屏的逻辑坐标：宽 64，高 128 */
#define V_W         64
#define V_H         128
#define SEG_T       8           /* 笔画粗细 */
#define DIGIT_H     108
#define DIGIT_Y0    ((V_H - DIGIT_H) / 2)
#define WIDE_W      44          /* 一位数 */
#define NARROW_W    26          /* 两位数 */
#define NARROW_GAP  6

static void fill(oled_fb_t fb, int x0, int y0, int w, int h)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++) {
            /* 逻辑坐标转到面板（横向 128x64）：转 90° */
            if (OLED_TL_ROTATE_180)
                oled_fb_pixel(fb, OLED_W - 1 - y, x, 1);
            else
                oled_fb_pixel(fb, y, OLED_H - 1 - x, 1);
        }
}

/* 七段：a 上、b 右上、c 右下、d 下、e 左下、f 左上、g 中 */
static void seven_seg(oled_fb_t fb, int x0, int w, unsigned segs)
{
    int y0 = DIGIT_Y0, h = DIGIT_H, half = h / 2, r = x0 + w - SEG_T;

    if (segs & 0x01) fill(fb, x0, y0, w, SEG_T);
    if (segs & 0x02) fill(fb, r, y0, SEG_T, half + SEG_T / 2);
    if (segs & 0x04) fill(fb, r, y0 + half - SEG_T / 2, SEG_T, h - half + SEG_T / 2);
    if (segs & 0x08) fill(fb, x0, y0 + h - SEG_T, w, SEG_T);
    if (segs & 0x10) fill(fb, x0, y0 + half - SEG_T / 2, SEG_T, h - half + SEG_T / 2);
    if (segs & 0x20) fill(fb, x0, y0, SEG_T, half + SEG_T / 2);
    if (segs & 0x40) fill(fb, x0, y0 + half - SEG_T / 2, w, SEG_T);
}

void oled_tl_render(oled_fb_t fb, int secs)
{
    static const unsigned char digit[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    int two_x = (V_W - 2 * NARROW_W - NARROW_GAP) / 2;

    oled_fb_clear(fb);
    if (secs < 0) {                             /* 优先放行中，不定时 */
        seven_seg(fb, two_x, NARROW_W, 0x40);
        seven_seg(fb, two_x + NARROW_W + NARROW_GAP, NARROW_W, 0x40);
    } else if (secs == 10) {
        seven_seg(fb, two_x, NARROW_W, digit[1]);
        seven_seg(fb, two_x + NARROW_W + NARROW_GAP, NARROW_W, digit[0]);
    } else if (secs >= 1 && secs <= 9) {
        seven_seg(fb, (V_W - WIDE_W) / 2, WIDE_W, digit[secs]);
    }
}
