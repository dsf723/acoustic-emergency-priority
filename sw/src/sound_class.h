/*
 * 声音识别（拍手 / 救护车警笛）与距离档位估计
 * 输入每一块 doa_process() 的结果，按规则判断声音类别，并粗略估计远近等级。
 * 不依赖硬件，可在 PC 上测试。
 * SC_TARGET_ONLY=1 为当前部署：仅输出 SND_NONE / SND_SIREN，尚非救护车专用分类器。
 *
 * 拍手：能量突增、宽带、几十毫秒内衰减；以起始块的方向为准。
 * 警笛：约 1 秒内持续的强音调（谱峰 400~2000Hz），谱峰频率有明显起伏（两音交替或连续升降）。
 * 远近：使用相干度连续窗口粗分近/中/远；不稳定或证据不足为未知，不输出米数。
 */
#ifndef SOUND_CLASS_H
#define SOUND_CLASS_H

#include <stdint.h>
#include "doa.h"

enum { SND_NONE = 0, SND_OTHER, SND_CLAP, SND_SIREN };
enum { BAND_NONE = 0, BAND_NEAR, BAND_MID, BAND_FAR };

typedef struct {
    int   cls;            /* 当前声音类别 SND_* */
    int   event;          /* 本块新确认了一次拍手，或警笛开始（1） */
    int   siren_active;   /* 警笛正在持续 */
    int   dir_ok;         /* 本块方向可用于显示（警笛块门限较低，见 SC_SIREN_MIN_PEAK_RATIO） */
    float event_az;       /* 拍手起始块的方位（event 且 cls == SND_CLAP 时有效，<0 表示未知） */
    float level_db;       /* 诊断电平（不用于计算米数） */
    int   band;           /* BAND_* */
    float distance_m;     /* 目标模式：按警笛音量估计的距离（米），<0 表示未知 */
} snd_state_t;

void        sc_init(void);
/* 处理一块；blk 为块序号（连续递增） */
void        sc_process(const doa_result_t *r, uint32_t blk, snd_state_t *out);
const char *sc_class_name(int cls);
const char *sc_band_name(int band);
/*
 * 目标模式的距离标定：把最近约 1 秒警笛的音量记为"距离 dist_m 处"的音量。
 * 警笛正在响、且已有足够的音量样本时返回 1，否则返回 0（标定值不变）。
 */
int         sc_range_calibrate(float dist_m);
float       sc_range_ref_db(void);          /* 当前 1 米处的参考音量（dB，相对值） */

#endif
