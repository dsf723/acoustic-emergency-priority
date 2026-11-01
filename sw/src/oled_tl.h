/*
 * OLED 信号灯倒计时（代替数码管）：屏幕竖着用，只显示七段数码管样式的数字
 *   1~10：显示秒数；大于 10 或 0：不显示；小于 0（优先放行中不定时）：显示 "--"
 * 数字上下颠倒时，改变 OLED_TL_ROTATE_180（0 / 1）。默认值按实际安装方向确认过。
 */
#ifndef OLED_TL_H
#define OLED_TL_H

#include "oled_ui.h"

#ifndef OLED_TL_ROTATE_180
#define OLED_TL_ROTATE_180  1
#endif

void oled_tl_render(oled_fb_t fb, int secs);

#endif
