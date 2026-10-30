/*
 * PC 端测试：与板上 main.c 相同的目标链路
 *   doa_process -> siren_focus_apply -> sc_process -> target_output_update
 * 用法：test_target_doa <包文件> <每包帧数> [标定块号 标定距离米]
 * 每行：序号 警笛 方向有效 目标方位 宽带原始方位 本块方位(聚焦后) 已锁定 锁定频率 远近档位 聚焦方位 聚焦峰值比
 *       警笛音量dB 估计距离m 标定结果(1成功/0失败/-1本块未标定)
 */
#include <stdio.h>
#include <stdlib.h>
#include "doa.h"
#include "sound_class.h"
#include "siren_focus.h"
#include "target_output.h"

int main(int argc, char **argv)
{
    FILE *fp;
    uint32_t *buf;
    int nframes, idx = 0, siren_prev = 0, cal_blk = -1, cal;
    float cal_m = 1.0f;
    doa_result_t r;
    snd_state_t sc = { 0 };
    target_output_t tgt = { 0 };
    siren_focus_info_t fi = { 0 };

    if (argc < 3)
        return 1;
    nframes = atoi(argv[2]);
    if (argc >= 5) {
        cal_blk = atoi(argv[3]);
        cal_m = (float)atof(argv[4]);
    }
    fp = fopen(argv[1], "rb");
    if (!fp)
        return 1;
    buf = (uint32_t *)malloc(sizeof(uint32_t) * 8 * nframes);
    doa_init();
    sc_init();
    siren_focus_reset();
    target_output_reset();
    r.target_level_db = -200.0f;
    while (fread(buf, sizeof(uint32_t), 8 * nframes, fp) == (size_t)(8 * nframes)) {
        if (doa_process(buf, nframes, &r) == 0) {
            siren_focus_apply(&r, siren_prev, &fi);
            sc_process(&r, (uint32_t)idx, &sc);
            doa_noise_freeze(sc.siren_active);
            target_output_update(&r, &sc, (uint32_t)idx, &tgt);
            siren_prev = sc.siren_active;
        }
        cal = -1;
        if (idx == cal_blk)
            cal = sc_range_calibrate(cal_m);
        printf("%d %d %d %.1f %.1f %.1f %d %.0f %d %.1f %.2f %.1f %.2f %d\n", idx++, tgt.active, tgt.direction_valid,
               tgt.azimuth_deg, fi.raw_azimuth, r.azimuth_deg, fi.locked, fi.track_hz, tgt.band,
               fi.focus_azimuth, fi.focus_peak_ratio, r.target_level_db, sc.distance_m, cal);
    }
    fclose(fp);
    free(buf);
    return 0;
}
