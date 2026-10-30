/*
 * SK9822 灯环驱动（12 颗），通过 AXI GPIO 软件模拟时钟/数据线
 * GPIO 通道 1：bit0 = LED_CK，bit1 = LED_DA
 */
#ifndef SK9822_H
#define SK9822_H

#include <stdint.h>
#include "xgpio.h"
#include "doa_config.h"

void sk9822_init(XGpio *gpio);
/* rgb[i] = {R, G, B}，brightness 0..31 */
void sk9822_show(const uint8_t rgb[LED_NUM][3], int brightness);
/* 按方位角点亮最近的 LED（绿色，相邻 LED 微亮）；angle < 0 时显示待机状态 */
void sk9822_show_direction(float angle_deg);
/* 同上，指定颜色 */
void sk9822_show_direction_rgb(float angle_deg, uint8_t r, uint8_t g, uint8_t b);

#endif
