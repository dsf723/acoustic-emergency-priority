/*
 * PC 端 OLED 竖屏倒计时画面测试：10、9、8、3、1、优先放行（--）、大于 10（空白）的帧缓冲写到文件，
 * 由 preview_oled_tl.py 转成 PNG
 * 用法：test_oled_tl <输出文件>
 */
#include <stdio.h>
#include "oled_tl.h"

static oled_fb_t fb;

int main(int argc, char **argv)
{
    static const int secs[] = { 10, 9, 8, 3, 1, -1, 15 };
    FILE *fp;
    unsigned i;

    if (argc < 2 || !(fp = fopen(argv[1], "wb")))
        return 1;
    for (i = 0; i < sizeof(secs) / sizeof(secs[0]); i++) {
        oled_tl_render(fb, secs[i]);
        fwrite(fb, sizeof(fb), 1, fp);
    }
    fclose(fp);
    return 0;
}
