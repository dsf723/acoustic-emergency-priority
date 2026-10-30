/*
 * HDMI 640x480 16 色中文界面（纯 C，不依赖硬件，可在 PC 上生成预览图）
 *
 * 帧缓冲格式与 PL 端 rtl/hdmi_fb.v 一致：每像素 4bit 颜色编号，每个 32bit 字 8 个像素，
 * 低 4bit 为最左边的像素；第 y 行第 x 个像素在字 y*80 + x/8。
 * 颜色编号对应 hdmi_fb.v 中的调色板（不改硬件）；文字为 2bit 抗锯齿点阵（hdmi_font.c，
 * 由 tools/gen_cjk_font.py 从界面字符串自动生成），边缘用调色板中相近的暗色过渡。
 */
#ifndef HDMI_UI_H
#define HDMI_UI_H

#include <stdint.h>
#include "oled_ui.h"            /* 复用显示状态结构 oled_ui_state_t 与远近档位定义 */
#include "hdmi_font.h"

#define HDMI_W          640
#define HDMI_H          480
#define HDMI_WORDS      (HDMI_W * HDMI_H / 8)

enum {
    HC_BG = 0, HC_WHITE, HC_DGRAY, HC_GRAY, HC_RED, HC_GREEN, HC_BLUE, HC_YELLOW,
    HC_CYAN, HC_MAGENTA, HC_ORANGE, HC_PANEL, HC_DGREEN, HC_DRED, HC_LGRAY, HC_BLACK
};

typedef uint32_t hdmi_fb_t[HDMI_WORDS];
typedef oled_ui_state_t hdmi_ui_state_t;

void hdmi_fill(hdmi_fb_t fb, int color);
void hdmi_pixel(hdmi_fb_t fb, int x, int y, int color);
void hdmi_rect(hdmi_fb_t fb, int x, int y, int w, int h, int color);
void hdmi_rrect(hdmi_fb_t fb, int x, int y, int w, int h, int r, int color);   /* 圆角矩形 */
void hdmi_disc(hdmi_fb_t fb, int cx, int cy, int r, int color);
void hdmi_ring(hdmi_fb_t fb, int cx, int cy, int r, int thick, int color);
void hdmi_line(hdmi_fb_t fb, int x0, int y0, int x1, int y1, int thick, int color);
void hdmi_tri(hdmi_fb_t fb, int x0, int y0, int x1, int y1, int x2, int y2, int color);
/* UTF-8 文字，(x, baseline) 为起点与基线；返回结束 x */
int  hdmi_str(hdmi_fb_t fb, const hfont_t *f, int x, int baseline, const char *s, int color);
int  hdmi_str_width(const hfont_t *f, const char *s);
/* 字库通用：取下一个 UTF-8 字符的码位；按码位找字形（没有返回 NULL）。OLED 也用 */
unsigned hfont_next_cp(const char **ps);
const hfont_glyph_t *hfont_find(const hfont_t *f, unsigned cp);
/* 自启动以来字库中缺字的次数（界面改字后忘记重新生成字库时 > 0，PC 测试检查） */
extern int hdmi_ui_missing_glyphs;

/*
 * 绘制完整界面：左侧识别目标 / 声源方位 / 距离估计，右侧罗盘（0° 朝上、逆时针为正，与阵列一致）。
 * mode_text：底部右侧的模式说明（UTF-8），可为 NULL。
 * 静态背景只在第一次调用时绘制并缓存，之后每次只画变化的部分。
 */
void hdmi_ui_render(hdmi_fb_t fb, const hdmi_ui_state_t *st, const char *mode_text);

#endif
