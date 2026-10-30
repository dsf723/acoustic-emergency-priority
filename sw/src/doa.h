/*
 * 声源方位估计（SRP-PHAT）与声音识别用的频谱特征
 * 输入：PL 送来的一个 DMA 包（mic_array_i2s_rx 的输出格式），输出声源方位角与特征。
 * 不依赖任何 Xilinx 库，可在 PC 上编译测试。
 */
#ifndef DOA_H
#define DOA_H

#include <stdint.h>

#define DOA_NSUB    32      /* 每块子帧数：每个子帧 32 个采样（约 0.66ms） */

typedef struct {
    int   valid;          /* 1：检测到声音且方向明确 */
    float azimuth_deg;    /* 方位角 0~360，阵列平面内，逆时针为正 */
    float energy_db;      /* 当前块定位频带能量（dB，相对值） */
    float noise_db;       /* 背景噪声估计（dB） */
    float peak_level_db;  /* 块内最大短时能量（约 2.6ms 帧，dBFS，不加窗），用于拍手等瞬态声音的响度 */
    float crest_db;       /* 峰均比：最强短时能量相对整块平均（dB），拍手高，语音/音乐/持续声低 */
    float sub_e[DOA_NSUB];/* 各子帧能量（预加重后、各麦平均，线性值），按时间顺序 */
    float peak_ratio;     /* SRP 峰值显著程度，越大方向越明确 */
    float coherence;      /* 最佳方向上的平均归一化相关值 0~1，直达声占比越高越大（近距离） */
    int   nbins;          /* 参与定位的频点数（频率掩码后） */
    /* 声音识别用特征 */
    float flatness;       /* 谱平坦度 0~1（300~8000Hz），宽带噪声/拍手高，纯音低 */
    float centroid_hz;    /* 谱质心（100~10000Hz） */
    float peak_hz;        /* 300~2500Hz 内最强谱峰频率 */
    float tonality_db;    /* 该谱峰相对该频段中位数的高度（dB），纯音/警笛高 */
    float peak_snr_db;    /* 该谱峰高于同一频点长时平均背景（约 5s，dB 域）的 dB 数：持续宽带噪声把整体信噪比
                             （energy_db - noise_db）压到接近 0 时，警笛的谱峰仍然突出；
                             房间里一直存在的嗡嗡声已被平均进背景，不会因此被当成警笛 */
    float ch_level_db[8]; /* 8 个 PL 通道的电平（dBFS），用于确认接线和通道映射 */
    float target_level_db;/* 目标（警笛）各谐波频点的总功率（dB，相对值），-200 表示本块没有；
                             由 siren_focus 填写，用于按音量估距离 */
} doa_result_t;

/* 初始化（计算几何、窗函数、FFT 旋转因子），程序开始时调用一次 */
void doa_init(void);

/*
 * 处理一个包。
 * pkt     ：DMA 缓冲区，nframes * 8 个 32bit 字
 * nframes ：包中帧数（第 0 帧可能是滞留旧帧，会被丢弃）
 * 返回 0 成功；-1 包内帧序号不连续或通道号错位（该包被丢弃，res 中只有电平有效）
 */
int doa_process(const uint32_t *pkt, int nframes, doa_result_t *res);

/* 目标频率聚焦定位结果（doa_focus） */
typedef struct {
    int   ok;             /* 1：找到目标分量且频点足够，方向有效 */
    float track_hz;       /* 本块找到的跟踪分量频率 */
    float f0_hz;          /* 推断的基频（跟踪分量或其 1/2、1/3） */
    float prominence_db;  /* 跟踪分量相对 300~2500Hz 中位数的高度，与 tonality_db 同一尺度 */
    float level_db;       /* 跟踪分量谱峰的绝对电平（同一声源各块之间比较响度是否平稳） */
    float snr_db;         /* 跟踪分量谱峰高于同一频点长时平均背景的 dB 数 */
    float power_db;       /* 参与定位的各谐波频点功率之和（dB，相对值） */
    float azimuth_deg;    /* 只用目标频点得到的方位 */
    float peak_ratio;
    float coherence;      /* 只用目标频点的相干度（远近档位用） */
    int   nbins;
} doa_focus_t;

typedef struct {
    float hz;
    float prominence_db;
} doa_peak_t;

/* 在 doa_process() 之后调用：列出 [lo_hz, hi_hz]（限 300~2500Hz）内突出度 >= min_prom_db 的谱峰 */
int doa_find_peaks(float lo_hz, float hi_hz, float min_prom_db, doa_peak_t *pk, int max_peaks);

/*
 * 在 doa_process() 之后调用：以谱峰 fp_hz（或其 1/2、1/3 处的基频）的各整数倍附近频点
 * 重新做 GCC-PHAT + SRP。返回 out->ok。
 */
int doa_focus_at(float fp_hz, doa_focus_t *out);

/* 冻结背景噪声估计（持续声音如警笛期间调用，避免噪声估计被抬高到声音本身的电平） */
void doa_noise_freeze(int freeze);

#endif
