/*
 * 警笛频率锁定 + 聚焦定位（抗干扰）
 *
 * 问题：宽带定位用的是本块所有高于背景的频点，干扰源（说话、敲击、音乐）比警笛响时，
 *       方向、远近（相干度）都会被干扰源拉走。
 * 做法：一旦出现像警笛的强音调就锁定其频率，之后每块只在锁定频率附近找谱峰
 *       （连续跟踪升降调、两音跳变），并只用其谐波频点定位（doa_focus）。
 *       用聚焦结果替换 doa_result_t 中的方位、峰值比、相干度、谱峰频率与音调度，
 *       下游的识别、方向滤波、远近档位与显示无需改动。
 *       锁定期间某块找不到目标分量时，该块不给方向（不退回宽带结果）；
 *       警笛确认后只接受聚焦方向。
 */
#ifndef SIREN_FOCUS_H
#define SIREN_FOCUS_H

#include "doa.h"

typedef struct {
    int   locked;         /* 已锁定目标频率 */
    int   focused;        /* 本块使用了聚焦结果 */
    float track_hz;       /* 当前锁定频率 */
    float raw_azimuth;    /* 宽带定位的原始方位（调试对比用） */
    float focus_azimuth;  /* 本块聚焦定位的方位（无论是否通过方向一致性检查），-1 表示无 */
    float focus_peak_ratio;
    float raw_peak_ratio;
} siren_focus_info_t;

void siren_focus_reset(void);
/*
 * 在 doa_process() 成功之后、sc_process() 之前调用。
 * siren_active：上一块 sc_process 输出的警笛状态。
 */
void siren_focus_apply(doa_result_t *r, int siren_active, siren_focus_info_t *info);

#endif
