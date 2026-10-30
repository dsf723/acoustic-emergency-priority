/*
 * PC 端界面测试：渲染几种典型状态，把帧缓冲写到文件，由 preview_oled.py 转成图片查看
 * 用法：test_oled_ui <输出文件>
 */
#include <stdio.h>
#include "oled_ui.h"

int main(int argc, char **argv)
{
    static const oled_ui_state_t cases[] = {
        { 1,   0.0f, "SIREN", OLED_BAND_NEAR, 1 },
        { 1, 215.0f, "SIREN", OLED_BAND_MID,  1 },
        { 1, 300.2f, "SIREN", OLED_BAND_FAR,  1 },
        { 1, 139.0f, "SIREN", OLED_BAND_NONE, 0 }, /* target present, angle unknown */
        { 0,   0.0f, "",      OLED_BAND_NONE, 0 },
    };
    oled_fb_t fb;
    FILE *fp;
    unsigned i;

    if (argc < 2)
        return 1;
    fp = fopen(argv[1], "wb");
    if (!fp)
        return 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        oled_ui_render(fb, &cases[i]);
        fwrite(fb, sizeof(fb), 1, fp);
    }
    fclose(fp);
    return 0;
}
