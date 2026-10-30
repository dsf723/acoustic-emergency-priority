/*
 * OLED 信号灯倒计时（代替数码管）：128x64 单色屏，左半南北、右半东西
 *   上：方向 + 当前灯色（绿/黄/红）  中：大号倒计时秒数（优先保持时 "--"）
 *   下：正常通行 / 救护车优先 + 来车方向
 * 中文用 HDMI 同一套字库（hdmi_font.c），覆盖度 >= 2 的像素点亮。
 */
#ifndef OLED_TL_H
#define OLED_TL_H

#include <stdint.h>
#include "oled_ui.h"
#include "traffic_light.h"

void oled_tl_render(oled_fb_t fb, const tl_state_t *tl, uint32_t now_ms);

#endif
