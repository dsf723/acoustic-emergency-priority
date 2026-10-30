/*
 * PC 端 OLED 信号灯倒计时画面测试：几种典型状态的帧缓冲写到文件，由 preview_oled_tl.py 转成 PNG
 * 用法：test_oled_tl <输出文件>
 */
#include <stdio.h>
#include "oled_tl.h"

static oled_fb_t fb;

static void step_to(tl_state_t *tl, uint32_t *now, uint32_t t, int req)
{
    while (*now < t) {
        *now += 20;
        tl_step(tl, *now, req);
    }
}

int main(int argc, char **argv)
{
    tl_state_t tl;
    uint32_t now = 0;
    FILE *fp;

    if (argc < 2 || !(fp = fopen(argv[1], "wb")))
        return 1;
    tl_init(&tl, 0);
    step_to(&tl, &now, 2500, -1);                 /* 正常：南北绿 18 秒 */
    oled_tl_render(fb, &tl, now);
    fwrite(fb, sizeof(fb), 1, fp);
    step_to(&tl, &now, 21500, -1);                /* 南北黄灯 */
    oled_tl_render(fb, &tl, now);
    fwrite(fb, sizeof(fb), 1, fp);
    tl_init(&tl, now);                            /* 南北绿灯第 3 秒，西口来救护车：等最短绿灯后早断 */
    step_to(&tl, &now, now + 3000, -1);
    step_to(&tl, &now, now + 500, TL_W);
    oled_tl_render(fb, &tl, now);
    fwrite(fb, sizeof(fb), 1, fp);
    step_to(&tl, &now, now + 9000, TL_W);         /* 东西绿灯保持 */
    oled_tl_render(fb, &tl, now);
    fwrite(fb, sizeof(fb), 1, fp);
    fclose(fp);
    return 0;
}
