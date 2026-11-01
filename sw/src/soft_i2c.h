/*
 * 软件模拟 I2C 主机（只写），用于东口、西口 OLED：PS 只有两个硬件 I2C 控制器。
 * 线路是开漏的：高电平靠释放总线、由上拉电阻拉高。不依赖硬件，引脚操作由回调提供（PC 测试 sw/test/test_soft_i2c.c）。
 */
#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include <stdint.h>

typedef struct {
    void (*scl)(void *ctx, int release);    /* release = 1 释放（高），0 拉低 */
    void (*sda)(void *ctx, int release);
    int  (*sda_read)(void *ctx);
    void (*delay)(void *ctx);               /* 半个时钟周期 */
    void *ctx;
} soft_i2c_t;

/* 发送 START、7 位地址 + 写、数据、STOP。全部应答返回 0；没有应答或总线被占住返回 -1 */
int soft_i2c_write(const soft_i2c_t *b, uint8_t addr, const uint8_t *buf, int len);

#endif
