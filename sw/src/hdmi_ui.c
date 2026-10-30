#include "hdmi_ui.h"
#include <math.h>
#include <string.h>

#define UI_PI       3.14159265f

/* 罗盘 */
#define CMP_CX      444
#define CMP_CY      252
#define CMP_R       150
#define CMP_BEAM_R  (CMP_R - 24)        /* 方向波束与指针的长度（不压住外圈刻度） */

/* 左侧三张卡片 */
#define CARD_X      24
#define CARD_W      208
#define CARD_R      10
#define CARD1_Y     72
#define CARD1_H     92
#define CARD2_Y     176
#define CARD2_H     156
#define CARD3_Y     344
#define CARD3_H     88

#define BAR_Y       448                 /* 底部状态栏 */

int hdmi_ui_missing_glyphs;

static hdmi_fb_t static_fb;             /* 静态背景（第一次渲染时画好） */
static int static_ok;

/* ---------------- 基本绘图 ---------------- */

void hdmi_fill(hdmi_fb_t fb, int color)
{
    uint32_t w = (uint32_t)(color & 15) * 0x11111111u;
    int i;
    for (i = 0; i < HDMI_WORDS; i++)
        fb[i] = w;
}

void hdmi_pixel(hdmi_fb_t fb, int x, int y, int color)
{
    uint32_t *p;
    int sh;
    if (x < 0 || x >= HDMI_W || y < 0 || y >= HDMI_H)
        return;
    p = &fb[y * (HDMI_W / 8) + (x >> 3)];
    sh = (x & 7) * 4;
    *p = (*p & ~(0xFu << sh)) | ((uint32_t)(color & 15) << sh);
}

/* 第 y 行 [x0, x1) 填色；整字直接写 */
static void hspan(hdmi_fb_t fb, int x0, int x1, int y, int color)
{
    uint32_t *row, w;
    if (y < 0 || y >= HDMI_H)
        return;
    if (x0 < 0) x0 = 0;
    if (x1 > HDMI_W) x1 = HDMI_W;
    if (x0 >= x1)
        return;
    row = &fb[y * (HDMI_W / 8)];
    w = (uint32_t)(color & 15) * 0x11111111u;
    while (x0 < x1 && (x0 & 7))
        hdmi_pixel(fb, x0++, y, color);
    while (x0 + 8 <= x1) {
        row[x0 >> 3] = w;
        x0 += 8;
    }
    while (x0 < x1)
        hdmi_pixel(fb, x0++, y, color);
}

void hdmi_rect(hdmi_fb_t fb, int x, int y, int w, int h, int color)
{
    int yy;
    for (yy = y; yy < y + h; yy++)
        hspan(fb, x, x + w, yy, color);
}

void hdmi_rrect(hdmi_fb_t fb, int x, int y, int w, int h, int r, int color)
{
    int yy;
    for (yy = 0; yy < h; yy++) {
        int inset = 0;
        float d = -1.0f;
        if (yy < r)
            d = (float)r - (float)yy - 0.5f;
        else if (yy >= h - r)
            d = (float)(yy - (h - r)) + 0.5f;
        if (d >= 0.0f)
            inset = r - (int)(sqrtf((float)(r * r) - d * d) + 0.5f);
        hspan(fb, x + inset, x + w - inset, y + yy, color);
    }
}

static int half_width(int r, int y)
{
    int v = r * r + r - y * y;
    return v < 0 ? -1 : (int)sqrtf((float)v);
}

void hdmi_disc(hdmi_fb_t fb, int cx, int cy, int r, int color)
{
    int y;
    for (y = -r; y <= r; y++) {
        int h = half_width(r, y);
        if (h >= 0)
            hspan(fb, cx - h, cx + h + 1, cy + y, color);
    }
}

void hdmi_ring(hdmi_fb_t fb, int cx, int cy, int r, int thick, int color)
{
    int ri = r - thick, y;
    for (y = -r; y <= r; y++) {
        int ho = half_width(r, y), hi = (y >= -ri && y <= ri) ? half_width(ri, y) : -1;
        if (ho < 0)
            continue;
        if (hi < 0) {
            hspan(fb, cx - ho, cx + ho + 1, cy + y, color);
        } else {
            hspan(fb, cx - ho, cx - hi, cy + y, color);
            hspan(fb, cx + hi + 1, cx + ho + 1, cy + y, color);
        }
    }
}

void hdmi_line(hdmi_fb_t fb, int x0, int y0, int x1, int y1, int thick, int color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int r = thick / 2;

    for (;;) {
        int e2;
        if (r > 0)
            hdmi_rect(fb, x0 - r, y0 - r, 2 * r + 1, 2 * r + 1, color);
        else
            hdmi_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void hdmi_tri(hdmi_fb_t fb, int x0, int y0, int x1, int y1, int x2, int y2, int color)
{
    const int px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
    int ymin = y0, ymax = y0, y, e;
    if (y1 < ymin) ymin = y1;
    if (y2 < ymin) ymin = y2;
    if (y1 > ymax) ymax = y1;
    if (y2 > ymax) ymax = y2;
    for (y = ymin; y <= ymax; y++) {
        float yc = (float)y + 0.5f, lo = 1e9f, hi = -1e9f;
        for (e = 0; e < 3; e++) {
            int a = e, b = (e + 1) % 3;
            float ya = (float)py[a], yb = (float)py[b];
            if ((yc >= ya && yc < yb) || (yc >= yb && yc < ya)) {
                float x = (float)px[a] + (yc - ya) * (float)(px[b] - px[a]) / (yb - ya);
                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
        }
        if (lo <= hi)
            hspan(fb, (int)(lo + 0.5f), (int)(hi + 0.5f) + 1, y, color);
    }
}

/* ---------------- 文字 ---------------- */

unsigned hfont_next_cp(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned c = s[0];
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else { c = '?'; n = 0; }
    s++;
    while (n-- > 0 && (*s & 0xC0) == 0x80)
        c = (c << 6) | (*s++ & 0x3F);
    *ps = (const char *)s;
    return c;
}

const hfont_glyph_t *hfont_find(const hfont_t *f, unsigned cp)
{
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->g[mid].cp == cp)
            return &f->g[mid];
        if (f->g[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* 抗锯齿边缘：覆盖度 1、2 用调色板里与前景色相近的暗色（-1 表示不画） */
static void edge_colors(int fg, int *c1, int *c2)
{
    switch (fg) {
    case HC_WHITE:
    case HC_LGRAY:  *c1 = HC_DGRAY; *c2 = HC_GRAY; break;
    case HC_GRAY:   *c1 = -1;       *c2 = HC_DGRAY; break;
    case HC_RED:    *c1 = HC_DRED;  *c2 = HC_RED; break;
    case HC_GREEN:  *c1 = HC_DGREEN; *c2 = HC_GREEN; break;
    case HC_DGRAY:  *c1 = -1;       *c2 = HC_DGRAY; break;
    default:        *c1 = HC_DGRAY; *c2 = fg; break;
    }
}

int hdmi_str(hdmi_fb_t fb, const hfont_t *f, int x, int baseline, const char *s, int color)
{
    int c1, c2;
    edge_colors(color, &c1, &c2);
    while (*s) {
        const hfont_glyph_t *g = hfont_find(f, hfont_next_cp(&s));
        const uint8_t *p;
        int stride, xx, yy;
        if (!g) {
            hdmi_ui_missing_glyphs++;
            x += f->height / 2;
            continue;
        }
        p = f->bits + g->off;
        stride = (g->w + 3) / 4;
        for (yy = 0; yy < g->h; yy++)
            for (xx = 0; xx < g->w; xx++) {
                int q = (p[yy * stride + (xx >> 2)] >> ((xx & 3) * 2)) & 3;
                int col = q == 3 ? color : q == 2 ? c2 : q == 1 ? c1 : -1;
                if (col >= 0)
                    hdmi_pixel(fb, x + g->xo + xx, baseline + g->yo + yy, col);
            }
        x += g->adv;
    }
    return x;
}

int hdmi_str_width(const hfont_t *f, const char *s)
{
    int w = 0;
    while (*s) {
        const hfont_glyph_t *g = hfont_find(f, hfont_next_cp(&s));
        w += g ? g->adv : f->height / 2;
    }
    return w;
}

/* ---------------- 界面 ---------------- */

/* 方位角 -> 屏幕坐标：0° 朝上，角度逆时针增加 */
static void polar(float az_deg, float r, int *x, int *y)
{
    float a = az_deg * UI_PI / 180.0f;
    *x = CMP_CX - (int)lroundf(r * sinf(a));
    *y = CMP_CY - (int)lroundf(r * cosf(a));
}

static const char *band_text(int band)
{
    switch (band) {
    case OLED_BAND_NEAR: return "近距离";
    case OLED_BAND_MID:  return "中距离";
    case OLED_BAND_FAR:  return "远距离";
    default:             return "未知";
    }
}

static int band_color(int band)
{
    switch (band) {
    case OLED_BAND_NEAR: return HC_GREEN;
    case OLED_BAND_MID:  return HC_YELLOW;
    case OLED_BAND_FAR:  return HC_ORANGE;
    default:             return HC_GRAY;
    }
}

static int band_level(int band)
{
    switch (band) {
    case OLED_BAND_NEAR: return 3;
    case OLED_BAND_MID:  return 2;
    case OLED_BAND_FAR:  return 1;
    default:             return 0;
    }
}

static void card(hdmi_fb_t fb, int y, int h, const char *label)
{
    hdmi_rrect(fb, CARD_X, y, CARD_W, h, CARD_R, HC_PANEL);
    hdmi_str(fb, &hfont_s, CARD_X + 16, y + 28, label, HC_GRAY);
}

static void draw_static(hdmi_fb_t fb)
{
    static const char *labels[4] = { "0°", "90°", "180°", "270°" };
    int k, x0, y0, x1, y1, d;

    hdmi_fill(fb, HC_BG);

    /* 标题栏 */
    x0 = hdmi_str(fb, &hfont_m, 24, 38, "声源定位系统", HC_WHITE);
    hdmi_str(fb, &hfont_s, x0 + 14, 37, "救护车警笛识别 · 七麦克风阵列", HC_GRAY);
    hdmi_rect(fb, 24, 56, HDMI_W - 48, 1, HC_DGRAY);

    /* 左侧卡片 */
    card(fb, CARD1_Y, CARD1_H, "识别目标");
    card(fb, CARD2_Y, CARD2_H, "声源方位");
    hdmi_str(fb, &hfont_s, CARD_X + 16, CARD2_Y + CARD2_H - 12, "0°为0号麦 · 逆时针为正", HC_DGRAY);
    card(fb, CARD3_Y, CARD3_H, "距离估计");

    /* 罗盘：底盘、同心圆、十字虚线、刻度、角度标注 */
    hdmi_disc(fb, CMP_CX, CMP_CY, CMP_R, HC_PANEL);
    hdmi_ring(fb, CMP_CX, CMP_CY, CMP_R, 2, HC_DGRAY);
    hdmi_ring(fb, CMP_CX, CMP_CY, CMP_R * 2 / 3, 1, HC_DGRAY);
    hdmi_ring(fb, CMP_CX, CMP_CY, CMP_R / 3, 1, HC_DGRAY);
    for (d = 40; d < CMP_R - 22; d += 5) {
        hdmi_pixel(fb, CMP_CX + d, CMP_CY, HC_DGRAY);
        hdmi_pixel(fb, CMP_CX - d, CMP_CY, HC_DGRAY);
        hdmi_pixel(fb, CMP_CX, CMP_CY + d, HC_DGRAY);
        hdmi_pixel(fb, CMP_CX, CMP_CY - d, HC_DGRAY);
    }
    for (d = 0; d < 360; d += 10) {
        int len = d % 90 == 0 ? 16 : d % 30 == 0 ? 11 : 6;
        int th = d % 90 == 0 ? 3 : d % 30 == 0 ? 2 : 1;
        int col = d % 90 == 0 ? HC_LGRAY : d % 30 == 0 ? HC_GRAY : HC_DGRAY;
        polar((float)d, (float)(CMP_R - 5), &x0, &y0);
        polar((float)d, (float)(CMP_R - 5 - len), &x1, &y1);
        hdmi_line(fb, x0, y0, x1, y1, th, col);
    }
    for (k = 0; k < 4; k++) {
        int w = hdmi_str_width(&hfont_s, labels[k]);
        polar(k * 90.0f, (float)CMP_R + (k % 2 ? 23 : 17), &x0, &y0);
        hdmi_str(fb, &hfont_s, x0 - w / 2, y0 + 6, labels[k], HC_GRAY);
    }

    /* 底部状态栏 */
    hdmi_rect(fb, 0, BAR_Y, HDMI_W, HDMI_H - BAR_Y, HC_PANEL);
    hdmi_rect(fb, 0, BAR_Y, HDMI_W, 1, HC_DGRAY);
}

/* 罗盘中心的阵列示意：6 个外圈麦克风（0 号为青色，即 0° 方向）+ 中心麦克风 */
static void draw_array(hdmi_fb_t fb)
{
    int k, x[6], y[6];
    hdmi_disc(fb, CMP_CX, CMP_CY, 30, HC_PANEL);
    for (k = 0; k < 6; k++)
        polar(k * 60.0f, 22.0f, &x[k], &y[k]);
    for (k = 0; k < 6; k++)
        hdmi_line(fb, x[k], y[k], x[(k + 1) % 6], y[(k + 1) % 6], 1, HC_DGRAY);
    for (k = 1; k < 6; k++)
        hdmi_disc(fb, x[k], y[k], 2, HC_GRAY);
    hdmi_disc(fb, x[0], y[0], 3, HC_CYAN);
    hdmi_disc(fb, CMP_CX, CMP_CY, 2, HC_GRAY);
}

static void draw_pointer(hdmi_fb_t fb, float az)
{
    int xa, ya, xb, yb, xt, yt;
    polar(az - 7.0f, (float)CMP_BEAM_R, &xa, &ya);
    polar(az + 7.0f, (float)CMP_BEAM_R, &xb, &yb);
    hdmi_tri(fb, CMP_CX, CMP_CY, xa, ya, xb, yb, HC_DRED);      /* 方向波束 */
    polar(az, (float)CMP_BEAM_R, &xt, &yt);
    hdmi_line(fb, CMP_CX, CMP_CY, xt, yt, 3, HC_RED);
    hdmi_disc(fb, xt, yt, 9, HC_RED);
    hdmi_disc(fb, xt, yt, 4, HC_WHITE);
}

static void utoa_s(char *buf, unsigned v)
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
}

void hdmi_ui_render(hdmi_fb_t fb, const hdmi_ui_state_t *st, const char *mode_text)
{
    int alarm = st->has_sound && st->cls_name && st->cls_name[0];
    int valid = alarm && st->direction_valid;
    int k, x, w;

    if (!static_ok) {
        draw_static(static_fb);
        static_ok = 1;
    }
    memcpy(fb, static_fb, sizeof(hdmi_fb_t));

    /* 右上角状态标签 */
    {
        const char *t = alarm ? "警报中" : "监听中";
        int pw = hdmi_str_width(&hfont_s, t) + 42, px = HDMI_W - 24 - pw;
        hdmi_rrect(fb, px, 14, pw, 28, 14, alarm ? HC_DRED : HC_PANEL);
        hdmi_disc(fb, px + 17, 28, 4, alarm ? HC_RED : HC_GREEN);
        hdmi_str(fb, &hfont_s, px + 29, 34, t, alarm ? HC_WHITE : HC_LGRAY);
    }

    /* 识别目标 */
    if (alarm) {
        hdmi_rect(fb, CARD_X, CARD1_Y + 12, 4, CARD1_H - 24, HC_RED);
        hdmi_str(fb, &hfont_l, CARD_X + 16, CARD1_Y + 74, "救护车警笛", HC_RED);
    } else {
        hdmi_str(fb, &hfont_l, CARD_X + 16, CARD1_Y + 74, "未检测到", HC_GRAY);
    }

    /* 声源方位：整数度；方向不确定时显示"未知"（识别结果仍有效） */
    if (valid) {
        char buf[16];
        utoa_s(buf, (unsigned)lroundf(st->azimuth_deg) % 360u);
        strcat(buf, "°");
        hdmi_str(fb, &hfont_xl, CARD_X + 14, CARD2_Y + 122, buf, HC_WHITE);
    } else if (alarm) {
        hdmi_str(fb, &hfont_l, CARD_X + 16, CARD2_Y + 86, "未知", HC_YELLOW);
        hdmi_str(fb, &hfont_s, CARD_X + 16, CARD2_Y + 114, "方向不确定，继续聆听", HC_GRAY);
    } else {
        hdmi_str(fb, &hfont_xl, CARD_X + 14, CARD2_Y + 122, "--", HC_DGRAY);
    }

    /* 距离估计：粗略档位 + 信号格 */
    if (alarm)
        hdmi_str(fb, &hfont_l, CARD_X + 16, CARD3_Y + 72, band_text(st->band), band_color(st->band));
    else
        hdmi_str(fb, &hfont_l, CARD_X + 16, CARD3_Y + 72, "--", HC_DGRAY);
    for (k = 0; k < 3; k++) {
        int h = 12 + 8 * k, bx = CARD_X + CARD_W - 16 - (3 - k) * 10 - (2 - k) * 6;
        int lit = alarm && k < band_level(st->band);
        hdmi_rect(fb, bx, CARD3_Y + 72 - h, 10, h, lit ? band_color(st->band) : HC_DGRAY);
    }
    /* 卡片右上角：按警笛音量估计的大约米数（需在 1 米处标定过才可信） */
    if (alarm && st->band != OLED_BAND_NONE && st->distance_m > 0.0f) {
        char buf[32], num[12];
        unsigned t = (unsigned)lroundf(st->distance_m * 10.0f);
        if (t > 999)
            t = 999;
        strcpy(buf, "约 ");
        utoa_s(num, t / 10);
        strcat(buf, num);
        strcat(buf, ".");
        utoa_s(num, t % 10);
        strcat(buf, num);
        strcat(buf, " 米");
        hdmi_str(fb, &hfont_s, CARD_X + CARD_W - 16 - hdmi_str_width(&hfont_s, buf), CARD3_Y + 28, buf, HC_LGRAY);
    }

    /* 罗盘：外圈颜色表示状态，方向有效时画波束与指针 */
    if (alarm)
        hdmi_ring(fb, CMP_CX, CMP_CY, CMP_R, 3, valid ? HC_RED : HC_YELLOW);
    if (valid)
        draw_pointer(fb, st->azimuth_deg);
    draw_array(fb);

    /* 底部状态栏 */
    hdmi_disc(fb, 24, BAR_Y + 16, 4, alarm ? HC_RED : HC_GREEN);
    hdmi_str(fb, &hfont_s, 36, BAR_Y + 22,
             alarm ? "检测到救护车警笛，请注意避让" : "系统运行中 · 等待救护车警笛",
             alarm ? HC_RED : HC_GRAY);
    if (mode_text) {
        w = hdmi_str_width(&hfont_s, mode_text);
        x = HDMI_W - 20 - w;
        hdmi_str(fb, &hfont_s, x, BAR_Y + 22, mode_text, HC_GRAY);
    }

    /* 检测到目标时屏幕四周红框 */
    if (alarm) {
        hdmi_rect(fb, 0, 0, HDMI_W, 3, HC_RED);
        hdmi_rect(fb, 0, HDMI_H - 3, HDMI_W, 3, HC_RED);
        hdmi_rect(fb, 0, 0, 3, HDMI_H, HC_RED);
        hdmi_rect(fb, HDMI_W - 3, 0, 3, HDMI_H, HC_RED);
    }
}
