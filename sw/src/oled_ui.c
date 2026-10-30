#include "oled_ui.h"
#include "font5x7.h"
#include "doa_config.h"
#include <math.h>
#include <string.h>

#define UI_PI       3.14159265f
#define DEG_CHAR    "\x7f"

/* 罗盘位置与半径 */
#define CMP_CX      96
#define CMP_CY      32
#define CMP_R       29

void oled_fb_clear(oled_fb_t fb)
{
    memset(fb, 0, sizeof(oled_fb_t));
}

void oled_fb_pixel(oled_fb_t fb, int x, int y, int on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H)
        return;
    if (on)
        fb[y >> 3][x] |= (uint8_t)(1u << (y & 7));
    else
        fb[y >> 3][x] &= (uint8_t)~(1u << (y & 7));
}

void oled_fb_line(oled_fb_t fb, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        int e2;
        oled_fb_pixel(fb, x0, y0, 1);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void oled_fb_circle(oled_fb_t fb, int cx, int cy, int r)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        oled_fb_pixel(fb, cx + x, cy + y, 1); oled_fb_pixel(fb, cx - x, cy + y, 1);
        oled_fb_pixel(fb, cx + x, cy - y, 1); oled_fb_pixel(fb, cx - x, cy - y, 1);
        oled_fb_pixel(fb, cx + y, cy + x, 1); oled_fb_pixel(fb, cx - y, cy + x, 1);
        oled_fb_pixel(fb, cx + y, cy - x, 1); oled_fb_pixel(fb, cx - y, cy - x, 1);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void oled_fb_fill_circle(oled_fb_t fb, int cx, int cy, int r)
{
    int x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r)
                oled_fb_pixel(fb, cx + x, cy + y, 1);
}

int oled_fb_text(oled_fb_t fb, int x, int y, const char *s, int scale)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int col, row, i, j;
        if (c < 32 || c > 127)
            c = '?';
        for (col = 0; col < 5; col++) {
            uint8_t bits = font5x7[c - 32][col];
            for (row = 0; row < 7; row++)
                if (bits & (1u << row))
                    for (i = 0; i < scale; i++)
                        for (j = 0; j < scale; j++)
                            oled_fb_pixel(fb, x + col * scale + i, y + row * scale + j, 1);
        }
        x += 6 * scale;
    }
    return x;
}

/* 非负整数转字符串，返回写入长度 */
static int utoa_s(char *buf, unsigned v)
{
    char tmp[12];
    int n = 0, i;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

static const char *band_name(int band)
{
    switch (band) {
    case OLED_BAND_NEAR: return "NEAR";
    case OLED_BAND_MID:  return "MID";
    case OLED_BAND_FAR:  return "FAR";
    default:             return "UNK";
    }
}

/* 方位角 -> 屏幕坐标：0° 朝上，角度逆时针增加 */
static void polar_to_xy(float az_deg, float r, int *x, int *y)
{
    float a = az_deg * UI_PI / 180.0f;
    *x = CMP_CX - (int)lroundf(r * sinf(a));
    *y = CMP_CY - (int)lroundf(r * cosf(a));
}

static void draw_compass(oled_fb_t fb, const oled_ui_state_t *st)
{
    int k, x0, y0, x1, y1;

    oled_fb_circle(fb, CMP_CX, CMP_CY, CMP_R);
    for (k = 0; k < 12; k++) {                      /* 每 30° 一个刻度，0° 刻度加长 */
        float az = k * 30.0f;
        polar_to_xy(az, (float)CMP_R, &x0, &y0);
        polar_to_xy(az, (float)(CMP_R - (k == 0 ? 6 : (k % 3 == 0 ? 4 : 2))), &x1, &y1);
        oled_fb_line(fb, x0, y0, x1, y1);
    }
    oled_fb_fill_circle(fb, CMP_CX, CMP_CY, 1);

    if (st->has_sound && st->direction_valid) {
        /* Fixed-length direction pointer: radius does not imply a distance. */
        float rp = (float)(CMP_R - 4);
        polar_to_xy(st->azimuth_deg, rp, &x1, &y1);
        oled_fb_line(fb, CMP_CX, CMP_CY, x1, y1);
        oled_fb_fill_circle(fb, x1, y1, 3);
    }
}

void oled_ui_render(oled_fb_t fb, const oled_ui_state_t *st)
{
    char buf[16];
    int x, n;

    oled_fb_clear(fb);

    /* 角度 */
    oled_fb_text(fb, 0, 0, "ANGLE", 1);
    if (st->has_sound && st->direction_valid) {
        unsigned a = (unsigned)lroundf(st->azimuth_deg) % 360u;
        utoa_s(buf, a);
        x = oled_fb_text(fb, 0, 10, buf, 2);
        oled_fb_text(fb, x, 10, DEG_CHAR, 1);
    } else if (st->has_sound) {
        oled_fb_text(fb, 0, 10, "UNK", 2);
    } else {
        oled_fb_text(fb, 0, 10, "---", 2);
    }

    /* Qualitative proximity only; UNK means insufficient or unstable evidence. */
    oled_fb_text(fb, 0, 29, "RANGE", 1);
    oled_fb_text(fb, 0, 39, st->has_sound ? band_name(st->band) : "---", 2);

    /* 底行：声音类别 + 距离档位，或等待提示 */
    if (st->has_sound) {
        n = 0;
        if (st->cls_name) {
            while (st->cls_name[n] && n < 6) {
                buf[n] = st->cls_name[n];
                n++;
            }
        }
        buf[n] = '\0';
        oled_fb_text(fb, 0, 57, buf, 1);
    } else {
        oled_fb_text(fb, 0, 57, "LISTENING", 1);
    }

    draw_compass(fb, st);
}
