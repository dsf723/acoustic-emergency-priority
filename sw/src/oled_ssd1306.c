#include "oled_ssd1306.h"
#include "xparameters.h"
#include "xiicps.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include <string.h>

#define OLED_ADDR       0x3C        /* 7 位地址（模块背面标 0x78 的即 0x3C） */
#define OLED_I2C_HZ     100000
#define OLED_TIMEOUT_US 20000       /* 单次传输最长等待时间 */
#define OLED_MAX_ERRORS 10          /* 连续出错次数上限，超过后停用 OLED */
#define HALF_W          (OLED_W / 2)

static XIicPs iic;
static int    oled_ok;
static int    err_count;

static int iic_setup(void)
{
    XIicPs_Config *cfg = XIicPs_LookupConfig(XPAR_XIICPS_0_DEVICE_ID);
    if (!cfg || XIicPs_CfgInitialize(&iic, cfg, cfg->BaseAddress) != XST_SUCCESS)
        return -1;
    XIicPs_Reset(&iic);
    return XIicPs_SetSClk(&iic, OLED_I2C_HZ) == XST_SUCCESS ? 0 : -1;
}

/* 等待总线空闲，超时返回 -1 */
static int wait_bus_idle(void)
{
    XTime t0, t;
    XTime_GetTime(&t0);
    while (XIicPs_BusIsBusy(&iic)) {
        XTime_GetTime(&t);
        if ((t - t0) > (XTime)OLED_TIMEOUT_US * (COUNTS_PER_SECOND / 1000000))
            return -1;
    }
    return 0;
}

static void report_error(void)
{
    /* 复位 I2C 控制器，释放可能卡住的总线状态 */
    iic_setup();
    if (++err_count >= OLED_MAX_ERRORS && oled_ok) {
        oled_ok = 0;
        xil_printf("OLED: %d consecutive I2C errors, display disabled (check SCL/SDA wiring)\r\n",
                   err_count);
    }
}

static int i2c_write(const u8 *buf, int len)
{
    if (wait_bus_idle() != 0 ||
        XIicPs_MasterSendPolled(&iic, (u8 *)buf, len, OLED_ADDR) != XST_SUCCESS ||
        wait_bus_idle() != 0) {
        report_error();
        return -1;
    }
    err_count = 0;
    return 0;
}

static int oled_cmds(const u8 *cmd, int n)
{
    u8 buf[32];
    buf[0] = 0x00;                  /* 控制字节：后续为命令 */
    memcpy(buf + 1, cmd, n);
    return i2c_write(buf, n + 1);
}

int oled_init(void)
{
    static const u8 init_seq[] = {
        0xAE,               /* 关显示 */
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

    oled_ok = 0;
    err_count = 0;
    if (iic_setup() != 0)
        return -1;
    if (oled_cmds(init_seq, sizeof(init_seq)) != 0)
        return -1;
    oled_ok = 1;
    return 0;
}

int oled_available(void)
{
    return oled_ok;
}

int oled_flush_step(const oled_fb_t fb, int step)
{
    u8 buf[1 + HALF_W];
    int page = step >> 1;
    int col0 = (step & 1) * HALF_W;
    const u8 addr[] = { 0x21, (u8)col0, (u8)(col0 + HALF_W - 1), 0x22, (u8)page, (u8)page };

    if (!oled_ok)
        return -1;
    if (oled_cmds(addr, sizeof(addr)) != 0)
        return -1;
    buf[0] = 0x40;                  /* 控制字节：后续为显示数据 */
    memcpy(buf + 1, &fb[page][col0], HALF_W);
    return i2c_write(buf, sizeof(buf));
}

int oled_flush(const oled_fb_t fb)
{
    int s;
    for (s = 0; s < OLED_FLUSH_STEPS; s++)
        if (oled_flush_step(fb, s) != 0)
            return -1;
    return 0;
}
