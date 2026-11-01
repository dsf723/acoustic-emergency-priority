/* soft_i2c.c 的 PC 测试：用引脚级的从机模型解码波形，并检查 I2C 时序规则 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "soft_i2c.h"

/* 从机模型：跟踪开漏总线上的 SCL / SDA，解码 START、字节、STOP，按需要应答 */
static struct {
    int scl_rel, sda_rel;           /* 主机是否释放 */
    int scl, sda;                   /* 上次的线电平 */
    int in_frame, bits, byte, ack_phase, slave_low;
    unsigned char rx[64];
    int rx_n, starts, stops;
    int nack_after;                 /* 收到这么多字节后不再应答，-1 一直应答 */
    int absent;                     /* 没有从机 */
    int stuck_clocks;               /* 还要多少个时钟才释放 SDA */
    int sda_changed_while_scl_high_in_data;
} s;

static int sda_line(void) { return s.sda_rel && !s.slave_low; }

static void update(void)
{
    int scl = s.scl_rel, sda = sda_line();

    if (s.scl && scl && s.sda != sda) {         /* SCL 高时 SDA 变化：START 或 STOP */
        /* STOP 前的 SCL 上升沿会被算成下一字节的第 1 位，已收到 2 位以上才是在字节中间改了 SDA */
        if (s.bits > 1)
            s.sda_changed_while_scl_high_in_data++;
        if (!sda) {
            s.starts++;
            s.in_frame = 1;
            s.bits = 0;
            s.byte = 0;
            s.ack_phase = 0;
        } else {
            s.stops++;
            s.in_frame = 0;
        }
    }
    if (!s.scl && scl) {                        /* 上升沿采样 */
        if (s.stuck_clocks > 0 && --s.stuck_clocks == 0)
            s.slave_low = 0;
        if (s.in_frame && !s.ack_phase) {
            s.byte = (s.byte << 1) | sda;
            s.bits++;
        }
    }
    if (s.scl && !scl && s.in_frame) {          /* 下降沿：驱动应答或结束应答位 */
        if (s.ack_phase) {
            s.ack_phase = 0;
            s.slave_low = 0;
            s.bits = 0;
            s.byte = 0;
        } else if (s.bits == 8) {
            s.rx[s.rx_n++] = (unsigned char)s.byte;
            s.ack_phase = 1;
            if (!s.absent && (s.nack_after < 0 || s.rx_n <= s.nack_after))
                s.slave_low = 1;
        }
    }
    s.scl = scl;
    s.sda = sda_line();
}

static void set_scl(void *c, int r) { (void)c; s.scl_rel = r; update(); }
static void set_sda(void *c, int r) { (void)c; s.sda_rel = r; update(); }
static int  get_sda(void *c) { (void)c; return sda_line(); }
static void delay(void *c) { (void)c; }

static const soft_i2c_t bus = { set_scl, set_sda, get_sda, delay, 0 };

static void reset(void)
{
    memset(&s, 0, sizeof(s));
    s.scl_rel = s.sda_rel = s.scl = s.sda = 1;
    s.nack_after = -1;
}

int main(void)
{
    static const unsigned char data[] = { 0x40, 0xFF, 0x00, 0xA5, 0x5A, 0x81 };

    /* 正常发送：地址 + 写 + 数据逐字节正确，一次 START、一次 STOP，结束后总线释放 */
    reset();
    assert(soft_i2c_write(&bus, 0x3C, data, sizeof(data)) == 0);
    assert(s.starts == 1 && s.stops == 1 && s.rx_n == (int)sizeof(data) + 1);
    assert(s.rx[0] == 0x78);
    assert(memcmp(s.rx + 1, data, sizeof(data)) == 0);
    assert(s.sda_changed_while_scl_high_in_data == 0);
    assert(s.scl_rel && s.sda_rel && sda_line());

    /* 连续两次传输 */
    assert(soft_i2c_write(&bus, 0x3C, data, 2) == 0);
    assert(s.starts == 2 && s.stops == 2 && s.rx_n == (int)sizeof(data) + 1 + 3);

    /* 没有从机：地址无应答，返回错误，不继续发数据，仍发 STOP */
    reset();
    s.absent = 1;
    assert(soft_i2c_write(&bus, 0x3C, data, sizeof(data)) == -1);
    assert(s.rx_n == 1 && s.stops == 1 && s.scl_rel && s.sda_rel);

    /* 从机中途不再应答：停止发送 */
    reset();
    s.nack_after = 2;
    assert(soft_i2c_write(&bus, 0x3C, data, sizeof(data)) == -1);
    assert(s.rx_n == 3 && s.stops == 1);

    /* 从机占住 SDA：补时钟后恢复，传输成功 */
    reset();
    s.slave_low = 1;
    s.sda = 0;
    s.stuck_clocks = 3;
    assert(soft_i2c_write(&bus, 0x3C, data, 2) == 0);
    assert(s.rx[0] == 0x78 && s.rx[1] == data[0] && s.rx[2] == data[1]);

    /* 一直占住：放弃，返回错误，没有 START */
    reset();
    s.slave_low = 1;
    s.sda = 0;
    s.stuck_clocks = 1000;
    assert(soft_i2c_write(&bus, 0x3C, data, 2) == -1);
    assert(s.starts == 0);

    puts("SOFT I2C PASSED: framing, data, ack, nack, absent slave, stuck SDA recovery");
    return 0;
}
