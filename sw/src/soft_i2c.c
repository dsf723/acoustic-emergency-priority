#include "soft_i2c.h"

static void clock_pulse(const soft_i2c_t *b)
{
    b->delay(b->ctx);
    b->scl(b->ctx, 1);
    b->delay(b->ctx);
    b->scl(b->ctx, 0);
}

/* 发送一个字节，收到应答返回 1 */
static int send_byte(const soft_i2c_t *b, uint8_t v)
{
    int i, ack;

    for (i = 7; i >= 0; i--) {
        b->sda(b->ctx, (v >> i) & 1);           /* SCL 低时才改变 SDA */
        clock_pulse(b);
    }
    b->sda(b->ctx, 1);                          /* 释放 SDA，由从机应答 */
    b->delay(b->ctx);
    b->scl(b->ctx, 1);
    b->delay(b->ctx);
    ack = !b->sda_read(b->ctx);
    b->scl(b->ctx, 0);
    return ack;
}

static void stop(const soft_i2c_t *b)
{
    b->sda(b->ctx, 0);
    b->delay(b->ctx);
    b->scl(b->ctx, 1);
    b->delay(b->ctx);
    b->sda(b->ctx, 1);
    b->delay(b->ctx);
}

int soft_i2c_write(const soft_i2c_t *b, uint8_t addr, const uint8_t *buf, int len)
{
    int i, ok;

    b->scl(b->ctx, 1);
    b->sda(b->ctx, 1);
    b->delay(b->ctx);
    if (!b->sda_read(b->ctx)) {
        /* 从机占住 SDA（上次传输被打断）：最多补 9 个时钟让它释放 */
        for (i = 0; i < 9 && !b->sda_read(b->ctx); i++) {
            b->scl(b->ctx, 0);
            clock_pulse(b);
            b->scl(b->ctx, 1);
            b->delay(b->ctx);
        }
        if (!b->sda_read(b->ctx))
            return -1;
    }
    b->sda(b->ctx, 0);                          /* START：SCL 高时 SDA 下降 */
    b->delay(b->ctx);
    b->scl(b->ctx, 0);

    ok = send_byte(b, (uint8_t)(addr << 1));
    for (i = 0; ok && i < len; i++)
        ok = send_byte(b, buf[i]);
    stop(b);
    return ok ? 0 : -1;
}
