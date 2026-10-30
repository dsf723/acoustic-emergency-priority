/*
 * 128x64 单色 OLED 帧缓冲绘图与声源定位界面（纯 C，不依赖硬件，可在 PC 上测试）
 * 帧缓冲格式与 SSD1306 页模式一致：fb[页 0..7][列 0..127]，每字节 8 行，bit0 在上
 */
#ifndef OLED_UI_H
#define OLED_UI_H

#include <stdint.h>

#define OLED_W      128
#define OLED_H      64
#define OLED_PAGES  (OLED_H / 8)

typedef uint8_t oled_fb_t[OLED_PAGES][OLED_W];

/* 距离档位，数值与 sound_class.h 中 BAND_* 一致 */
#define OLED_BAND_NONE  0
#define OLED_BAND_NEAR  1
#define OLED_BAND_MID   2
#define OLED_BAND_FAR   3

typedef struct {
    int         has_sound;      /* 1：最近检测到声源 */
    float       azimuth_deg;    /* 方位角 0~360 */
    const char *cls_name;       /* 声音类别："CLAP" / "SIREN" / "SOUND" */
    int         band;           /* OLED_BAND_* */
    int         direction_valid;/* 与声音类别独立：0 时显示角度 UNK 并隐藏指针 */
    float       distance_m;     /* 按警笛音量估计的距离（米），<= 0 表示未知（HDMI 显示"约 x.x 米"） */
} oled_ui_state_t;

void oled_fb_clear(oled_fb_t fb);
void oled_fb_pixel(oled_fb_t fb, int x, int y, int on);
void oled_fb_line(oled_fb_t fb, int x0, int y0, int x1, int y1);
void oled_fb_circle(oled_fb_t fb, int cx, int cy, int r);
void oled_fb_fill_circle(oled_fb_t fb, int cx, int cy, int r);
/* scale = 1 时每字 6x8 像素（含间隔），scale = 2 时 12x16 */
int  oled_fb_text(oled_fb_t fb, int x, int y, const char *s, int scale);

/* 绘制完整界面：左侧角度/距离/类别，右侧罗盘（指针 + 声源位置点） */
void oled_ui_render(oled_fb_t fb, const oled_ui_state_t *st);

#endif
