#include "sk9822.h"

#define LED_CK_BIT  0x1
#define LED_DA_BIT  0x2

static XGpio *led_gpio;

static void send_byte(uint8_t b)
{
    int i;
    for (i = 7; i >= 0; i--) {
        uint32_t da = (b >> i) & 1 ? LED_DA_BIT : 0;
        XGpio_DiscreteWrite(led_gpio, 1, da);               /* CK 低，数据建立 */
        XGpio_DiscreteWrite(led_gpio, 1, da | LED_CK_BIT);  /* CK 上升沿锁存 */
    }
}

void sk9822_init(XGpio *gpio)
{
    led_gpio = gpio;
    XGpio_SetDataDirection(led_gpio, 1, 0x0);
    XGpio_DiscreteWrite(led_gpio, 1, 0);
}

void sk9822_show(const uint8_t rgb[LED_NUM][3], int brightness)
{
    int i;
    if (brightness < 0)  brightness = 0;
    if (brightness > 31) brightness = 31;

    for (i = 0; i < 4; i++)                     /* 起始帧：32 个 0 */
        send_byte(0x00);
    for (i = 0; i < LED_NUM; i++) {
        send_byte(0xE0 | brightness);
        send_byte(rgb[i][2]);                   /* B */
        send_byte(rgb[i][1]);                   /* G */
        send_byte(rgb[i][0]);                   /* R */
    }
    for (i = 0; i < 4; i++)                     /* 结束帧：32 个 1 */
        send_byte(0xFF);
    XGpio_DiscreteWrite(led_gpio, 1, 0);
}

void sk9822_show_direction_rgb(float angle_deg, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rgb[LED_NUM][3] = {{0}};
    int i;

    if (angle_deg < 0.0f) {
        for (i = 0; i < LED_NUM; i++)
            rgb[i][2] = 8;                      /* 待机：微弱蓝光 */
    } else {
        float pos = (angle_deg - LED0_ANGLE_DEG) * LED_DIR / (360.0f / LED_NUM);
        int idx = (int)(pos + 0.5f + LED_NUM * 4) % LED_NUM;
        int nb[2] = { (idx + 1) % LED_NUM, (idx + LED_NUM - 1) % LED_NUM };
        rgb[idx][0] = r;
        rgb[idx][1] = g;
        rgb[idx][2] = b;
        for (i = 0; i < 2; i++) {               /* 相邻 LED 微亮 */
            rgb[nb[i]][0] = r / 10;
            rgb[nb[i]][1] = g / 10;
            rgb[nb[i]][2] = b / 10;
        }
    }
    sk9822_show(rgb, LED_BRIGHTNESS);
}

void sk9822_show_direction(float angle_deg)
{
    sk9822_show_direction_rgb(angle_deg, 0, 255, 0);
}
