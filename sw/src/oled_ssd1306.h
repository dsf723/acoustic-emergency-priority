/*
 * 0.96 寸 SSD1306 OLED（128x64，I2C 地址 0x3C）驱动
 * 使用 PS I2C0（经 EMIO 引到 PL 引脚 oled_iic_scl_io / oled_iic_sda_io），100kHz
 *
 * 100kHz 下整屏刷新约需 95ms，远超一个音频块（21ms）的处理时间预算，
 * 因此按半页增量刷新：每次调用 oled_flush_step() 只发送 64 字节（约 6ms），
 * OLED_FLUSH_STEPS 次刷新一整屏。
 *
 * 所有 I2C 等待都有超时；出错时复位 I2C 控制器并跳过本次发送，
 * 连续出错 OLED_MAX_ERRORS 次后停用 OLED，保证定位主循环不会被显示卡住。
 */
#ifndef OLED_SSD1306_H
#define OLED_SSD1306_H

#include "oled_ui.h"

#define OLED_FLUSH_STEPS    (OLED_PAGES * 2)

/* 初始化 I2C 与屏幕，成功返回 0；屏幕未应答返回 -1 */
int  oled_init(void);
/* 发送帧缓冲的第 step 个半页（0..OLED_FLUSH_STEPS-1） */
int  oled_flush_step(const oled_fb_t fb, int step);
/* 发送整屏（阻塞约 0.1s，用于启动画面） */
int  oled_flush(const oled_fb_t fb);
/* OLED 是否可用（初始化成功且未因连续出错被停用） */
int  oled_available(void);

#endif
