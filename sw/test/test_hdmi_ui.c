/*
 * PC 端 HDMI 界面测试：渲染几种典型状态，把帧缓冲写到文件，由 preview_hdmi.py 转成 PNG
 * 用法：test_hdmi_ui <输出文件>
 */
#include <stdio.h>
#include "hdmi_ui.h"

static hdmi_fb_t fb;

int main(int argc, char **argv)
{
    static const hdmi_ui_state_t cases[] = {
        { 1,  37.0f, "SIREN", OLED_BAND_NEAR, 1, 0.8f },     /* 警笛，方向有效，约 0.8 米 */
        { 1, 215.0f, "SIREN", OLED_BAND_FAR,  1, 5.3f },
        { 1,   0.0f, "SIREN", OLED_BAND_NONE, 0 },     /* 警笛，方向未知 */
        { 0,   0.0f, "",      OLED_BAND_NONE, 0 },     /* 无目标 */
    };
    FILE *fp;
    unsigned i;

    if (argc < 2)
        return 1;
    fp = fopen(argv[1], "wb");
    if (!fp)
        return 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hdmi_ui_render(fb, &cases[i], i < 2 ? "救护车优先 · 放行西口" : "南北 绿 18 · 东西 红 23");
        fwrite(fb, sizeof(fb), 1, fp);
    }
    fclose(fp);
    if (hdmi_ui_missing_glyphs) {
        printf("missing glyphs: %d (run tools/gen_cjk_font.py after changing UI text)\n", hdmi_ui_missing_glyphs);
        return 2;
    }
    return 0;
}
