/*
 * PC 端测试：读取 gen_test.py 生成的包文件，逐包运行 doa_process + sc_process 并打印结果
 * 用法：test_doa <包文件> <每包帧数>
 * 每行：序号 ret valid 方位 峰值比 能量 噪声 相干度 平坦度 质心 谱峰 音调 类别 事件 警笛 保留列(-1) 档位 方向可用 事件方位 短时峰值电平 峰均比
 */
#include <stdio.h>
#include <stdlib.h>
#include "doa.h"
#include "sound_class.h"

int main(int argc, char **argv)
{
    FILE *fp;
    uint32_t *buf;
    int nframes, idx = 0;
    doa_result_t r;
    snd_state_t sc;

    if (argc < 3) {
        fprintf(stderr, "usage: %s pkt.bin nframes\n", argv[0]);
        return 1;
    }
    nframes = atoi(argv[2]);
    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }
    buf = (uint32_t *)malloc(sizeof(uint32_t) * 8 * nframes);
    doa_init();
    sc_init();
    while (fread(buf, sizeof(uint32_t), 8 * nframes, fp) == (size_t)(8 * nframes)) {
        int ret = doa_process(buf, nframes, &r);
        if (ret == 0) {
            sc_process(&r, (uint32_t)idx, &sc);
            doa_noise_freeze(sc.siren_active);
        }
        printf("%d %d %d %.2f %.2f %.2f %.2f %.3f %.3f %.0f %.1f %.1f %d %d %d %.2f %d %d %.1f %.2f %.2f\n",
               idx++, ret, r.valid, r.azimuth_deg, r.peak_ratio, r.energy_db, r.noise_db,
               r.coherence, r.flatness, r.centroid_hz, r.peak_hz, r.tonality_db,
               sc.cls, sc.event, sc.siren_active, -1.0f, sc.band, sc.dir_ok, sc.event_az,
               r.peak_level_db, r.crest_db);
    }
    fclose(fp);
    free(buf);
    return 0;
}
