#include "oled_ssd1306.h"
#include "soft_i2c.h"
#include "xparameters.h"
#include "xgpio.h"
#include "xiicps.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include <string.h>

#define OLED_ADDR       0x3C        /* 7 位地址（模块背面标 0x78 的即 0x3C） */
#define OLED_I2C_HZ     400000
#define OLED_TIMEOUT_US 3000        /* 单次传输最长等待时间（64 字节约 1.5ms）；总线被占住时不能拖住主循环 */
#define HALF_W          (OLED_W / 2)
#define OLED_HALF_PAGES (OLED_PAGES * 2)
#define NUM_PS_I2C      2           /* 前两块用 PS 硬件 I2C，其余用软件 I2C */

typedef struct {
    int       ok;
    int       pos;                  /* 下一个要比较的半页 */
    oled_fb_t sent;                 /* 屏幕现有内容 */
} oled_t;

static oled_t  oled[OLED_NUM];
static XIicPs  ps_iic[NUM_PS_I2C];
static const char *const oled_label[OLED_NUM] = { "N", "S", "E", "W" };

/* ---------------- 软件 I2C：GPIO 输出寄存器恒为 0，方向位 1 = 释放（上拉为高），0 = 拉低 ---------------- */
static XGpio   gpio_oled;
static u32     gpio_tri = 0xFu;

static void pin_set(int bit, int release)
{
    if (release)
        gpio_tri |= 1u << bit;
    else
        gpio_tri &= ~(1u << bit);
    XGpio_SetDataDirection(&gpio_oled, 1, gpio_tri);
}

/* ctx 是该路 SCL 的位号，SDA 为下一位 */
static void sw_scl(void *ctx, int release) { pin_set((int)(INTPTR)ctx, release); }
static void sw_sda(void *ctx, int release) { pin_set((int)(INTPTR)ctx + 1, release); }

static int sw_sda_read(void *ctx)
{
    return (XGpio_DiscreteRead(&gpio_oled, 1) >> ((int)(INTPTR)ctx + 1)) & 1u;
}

/* 半个时钟周期不小于 1.25us（400kHz），GPIO 写本身的时间也算在内 */
static void sw_delay(void *ctx)
{
    XTime t0, t;
    (void)ctx;
    XTime_GetTime(&t0);
    do {
        XTime_GetTime(&t);
    } while ((t - t0) < (XTime)(COUNTS_PER_SECOND / (2 * OLED_I2C_HZ)));
}

static const soft_i2c_t sw_bus[OLED_NUM - NUM_PS_I2C] = {
    { sw_scl, sw_sda, sw_sda_read, sw_delay, (void *)(INTPTR)0 },
    { sw_scl, sw_sda, sw_sda_read, sw_delay, (void *)(INTPTR)2 },
};

/* ---------------- PS 硬件 I2C ---------------- */
static int iic_setup(int n)
{
    static const u16 dev_id[NUM_PS_I2C] = { XPAR_XIICPS_0_DEVICE_ID, XPAR_XIICPS_1_DEVICE_ID };
    XIicPs_Config *cfg = XIicPs_LookupConfig(dev_id[n]);
    if (!cfg || XIicPs_CfgInitialize(&ps_iic[n], cfg, cfg->BaseAddress) != XST_SUCCESS)
        return -1;
    XIicPs_Reset(&ps_iic[n]);
    return XIicPs_SetSClk(&ps_iic[n], OLED_I2C_HZ) == XST_SUCCESS ? 0 : -1;
}

/* 等待总线空闲，超时返回 -1 */
static int wait_bus_idle(int n)
{
    XTime t0, t;
    XTime_GetTime(&t0);
    while (XIicPs_BusIsBusy(&ps_iic[n])) {
        XTime_GetTime(&t);
        if ((t - t0) > (XTime)OLED_TIMEOUT_US * (COUNTS_PER_SECOND / 1000000))
            return -1;
    }
    return 0;
}

/* ---------------- 公共 ---------------- */
static int i2c_write(int n, const u8 *buf, int len)
{
    int ret;

    if (n < NUM_PS_I2C)
        ret = (wait_bus_idle(n) != 0 ||
               XIicPs_MasterSendPolled(&ps_iic[n], (u8 *)buf, len, OLED_ADDR) != XST_SUCCESS ||
               wait_bus_idle(n) != 0) ? -1 : 0;
    else
        ret = soft_i2c_write(&sw_bus[n - NUM_PS_I2C], OLED_ADDR, buf, len);
    if (ret != 0) {
        /* 复位 I2C 控制器，释放可能卡住的总线状态；之后由 oled_maintain() 恢复 */
        if (n < NUM_PS_I2C)
            iic_setup(n);
        if (oled[n].ok) {
            oled[n].ok = 0;
            xil_printf("OLED %s lost (check wiring), retrying\r\n", oled_label[n]);
        }
        return -1;
    }
    return 0;
}

static int oled_cmds(int n, const u8 *cmd, int len)
{
    u8 buf[32];
    buf[0] = 0x00;                  /* 控制字节：后续为命令 */
    memcpy(buf + 1, cmd, len);
    return i2c_write(n, buf, len + 1);
}

/* 全部设置都可以重复发送；没有"关显示"，运行中重发不会闪屏 */
static int send_init(int n)
{
    static const u8 init_seq[] = {
        0xD5, 0x80,         /* 时钟分频 */
        0xA8, 0x3F,         /* 多路复用 1/64 */
        0xD3, 0x00,         /* 显示偏移 0 */
        0x40,               /* 起始行 0 */
        0x8D, 0x14,         /* 开电荷泵 */
        0x20, 0x00,         /* 水平寻址模式 */
        0xA1,               /* 列反向（模块常见安装方向） */
        0xC8,               /* 行扫描反向 */
        0xDA, 0x12,         /* COM 引脚配置 */
        0x81, 0xCF,         /* 对比度 */
        0xD9, 0xF1,         /* 预充电周期 */
        0xDB, 0x40,         /* VCOMH */
        0xA4,               /* 显示 RAM 内容 */
        0xA6,               /* 正常显示（非反色） */
        0xAF,               /* 开显示 */
    };
    return oled_cmds(n, init_seq, sizeof(init_seq));
}

int oled_init(void)
{
    int n, count = 0;

    XGpio_Initialize(&gpio_oled, XPAR_AXI_GPIO_OLED_DEVICE_ID);
    XGpio_DiscreteWrite(&gpio_oled, 1, 0);
    gpio_tri = 0xFu;
    XGpio_SetDataDirection(&gpio_oled, 1, gpio_tri);

    for (n = 0; n < OLED_NUM; n++) {
        oled[n].ok = 0;
        oled[n].pos = 0;
        if (n < NUM_PS_I2C && iic_setup(n) != 0)
            continue;
        if (send_init(n) != 0)
            continue;
        oled[n].ok = 1;
        count++;
    }
    return count;
}

int oled_maintain(int n, const oled_fb_t fb)
{
    oled_t *d = &oled[n];
    int was_ok = d->ok, page, col;

    if (send_init(n) != 0)
        return 0;
    if (!was_ok)
        xil_printf("OLED %s recovered\r\n", oled_label[n]);
    d->ok = 1;
    /* 屏幕内容未知：让 sent 与 fb 处处不同，oled_sync_step() 就会整屏重发 */
    for (page = 0; page < OLED_PAGES; page++)
        for (col = 0; col < OLED_W; col++)
            d->sent[page][col] = (u8)~fb[page][col];
    return 1;
}

int oled_ok(int n)
{
    return oled[n].ok;
}

int oled_available(void)
{
    int n;
    for (n = 0; n < OLED_NUM; n++)
        if (oled[n].ok)
            return 1;
    return 0;
}

/* 发送第 page 页的 [c0, c1] 列，成功后记入 sent */
static int send_columns(int n, const oled_fb_t fb, int page, int c0, int c1)
{
    u8 buf[1 + HALF_W];
    int len = c1 - c0 + 1;
    const u8 addr[] = { 0x21, (u8)c0, (u8)c1, 0x22, (u8)page, (u8)page };

    if (oled_cmds(n, addr, sizeof(addr)) != 0)
        return -1;
    buf[0] = 0x40;                  /* 控制字节：后续为显示数据 */
    memcpy(buf + 1, &fb[page][c0], len);
    if (i2c_write(n, buf, len + 1) != 0)
        return -1;
    memcpy(&oled[n].sent[page][c0], &fb[page][c0], len);
    return 0;
}

int oled_sync_step(int n, const oled_fb_t fb)
{
    oled_t *d = &oled[n];
    int i;

    if (!d->ok)
        return 0;
    for (i = 0; i < OLED_HALF_PAGES; i++) {
        int page = d->pos >> 1, col0 = (d->pos & 1) * HALF_W;
        int c0 = col0, c1 = col0 + HALF_W - 1;
        d->pos = (d->pos + 1) % OLED_HALF_PAGES;
        while (c0 <= c1 && fb[page][c0] == d->sent[page][c0])
            c0++;
        if (c0 > c1)
            continue;
        while (fb[page][c1] == d->sent[page][c1])
            c1--;
        send_columns(n, fb, page, c0, c1);
        return 1;
    }
    return 0;
}

int oled_flush(int n, const oled_fb_t fb)
{
    int page, col;

    if (!oled[n].ok)
        return -1;
    for (page = 0; page < OLED_PAGES; page++)
        for (col = 0; col < OLED_W; col += HALF_W)
            if (send_columns(n, fb, page, col, col + HALF_W - 1) != 0)
                return -1;
    return 0;
}
