#include "oled_tl.h"
#include "hdmi_ui.h"            /* hfont_find / hfont_next_cp 与字库 */
#include <string.h>

/* UTF-8 文字画到 OLED（单色：覆盖度 >= 2 点亮），返回结束 x */
static int oled_str(oled_fb_t fb, const hfont_t *f, int x, int baseline, const char *s)
{
    while (*s) {
        const hfont_glyph_t *g = hfont_find(f, hfont_next_cp(&s));
        const uint8_t *p;
        int stride, xx, yy;
        if (!g) {
            x += f->height / 2;
            continue;
        }
        p = f->bits + g->off;
        stride = (g->w + 3) / 4;
        for (yy = 0; yy < g->h; yy++)
            for (xx = 0; xx < g->w; xx++)
                if (((p[yy * stride + (xx >> 2)] >> ((xx & 3) * 2)) & 3) >= 2)
                    oled_fb_pixel(fb, x + g->xo + xx, baseline + g->yo + yy, 1);
        x += g->adv;
    }
    return x;
}

static int oled_str_width(const hfont_t *f, const char *s)
{
    int w = 0;
    while (*s) {
        const hfont_glyph_t *g = hfont_find(f, hfont_next_cp(&s));
        w += g ? g->adv : f->height / 2;
    }
    return w;
}

static const char *light_name(int l)
{
    return l == TL_GREEN ? "绿" : l == TL_YELLOW ? "黄" : "红";
}

/* 一个相位的半屏：x0 起 64 像素宽 */
static void half(oled_fb_t fb, int x0, const char *name, int light, int secs)
{
    char buf[8];
    int x, w;

    x = oled_str(fb, &hfont_s, x0 + 4, 15, name);
    oled_str(fb, &hfont_s, x + 4, 15, light_name(light));
    if (light == TL_GREEN) {                    /* 绿灯：灯色字下加横线，远处也一眼看出谁在放行 */
        int i;
        for (i = 0; i < 16; i++)
            oled_fb_pixel(fb, x + 4 + i, 17, 1);
    }
    if (secs < 0) {
        strcpy(buf, "--");
    } else {
        if (secs > 99)
            secs = 99;
        buf[0] = (char)('0' + secs / 10);
        buf[1] = (char)('0' + secs % 10);
        buf[2] = '\0';
        if (secs < 10) {
            buf[0] = buf[1];
            buf[1] = '\0';
        }
    }
    w = oled_str_width(&hfont_l, buf);
    oled_str(fb, &hfont_l, x0 + (64 - w) / 2, 45, buf);
}

void oled_tl_render(oled_fb_t fb, const tl_state_t *tl, uint32_t now_ms)
{
    int y;

    oled_fb_clear(fb);
    half(fb, 0, "南北", tl_light(tl, TL_N), tl_countdown_s(tl, TL_PH_NS, now_ms));
    half(fb, 64, "东西", tl_light(tl, TL_E), tl_countdown_s(tl, TL_PH_EW, now_ms));
    for (y = 0; y < 47; y += 2)
        oled_fb_pixel(fb, 63, y, 1);
    if (tl->preempt) {
        char buf[48];
        strcpy(buf, "救护车优先 ");
        strcat(buf, tl_approach_name(tl->approach));
        strcat(buf, "口");
        oled_str(fb, &hfont_s, (OLED_W - oled_str_width(&hfont_s, buf)) / 2, 63, buf);
    } else {
        oled_str(fb, &hfont_s, (OLED_W - oled_str_width(&hfont_s, "正常通行")) / 2, 63, "正常通行");
    }
}
