/*
 * 4 块 0.96 寸 SSD1306 OLED（128x64，I2C 地址都是 0x3C）驱动，每块一条独立的 I2C，400kHz
 *   0 北口：PS I2C0（oled_iic_*，J4.8/10）
 *   1 南口：PS I2C1（oled2_iic_*，J4.25/26）
 *   2 东口：AXI GPIO 软件 I2C（oled_sw_tri_io[1:0]，J4.29/30）
 *   3 西口：AXI GPIO 软件 I2C（oled_sw_tri_io[3:2]，J4.31/32）
 * 编号与 traffic_light.h 的 TL_N/S/E/W 一致。
 *
 * 整屏刷新约 25ms，超过一个音频块（21ms）的处理时间预算，因此增量刷新：
 * 每次调用 oled_sync_step() 只发送一个半页中有变化的列（最多 64 字节，约 1.5ms）。
 *
 * 所有 I2C 等待都有超时；出错时跳过本次发送，
 * 某块屏连续出错 OLED_MAX_ERRORS 次后停用，保证定位主循环不会被显示卡住。
 */
#ifndef OLED_SSD1306_H
#define OLED_SSD1306_H

#include "oled_ui.h"

#define OLED_NUM    4

/* 初始化 4 条总线和屏幕，返回应答的屏幕数 */
int  oled_init(void);
/* 发送整屏（阻塞约 25ms，用于启动画面） */
int  oled_flush(int n, const oled_fb_t fb);
/* 发送一个有变化的半页；发送了返回 1，没有变化或该屏不可用返回 0 */
int  oled_sync_step(int n, const oled_fb_t fb);
/* 该屏是否可用（初始化成功且未因连续出错被停用） */
int  oled_ok(int n);
/* 至少有一块屏可用 */
int  oled_available(void);

#endif
