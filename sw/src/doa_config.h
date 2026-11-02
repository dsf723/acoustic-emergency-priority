/*
 * 声源定位参数配置
 * 上板后需要根据实际情况标定的参数都在这里，改完重新编译即可。
 */
#ifndef DOA_CONFIG_H
#define DOA_CONFIG_H

/* ---------------- 采样 ---------------- */
/* PL 时钟 50MHz，SCK = 50M/16 = 3.125MHz，fs = SCK/64 */
#define DOA_FS_HZ           48828.125f
/* 每个 DMA 包的帧数，必须与 PL 中 mic_array_i2s_rx 的 FRAMES_PER_PKT 一致 */
#define DOA_FRAMES_PER_PKT  1024
/* FFT 点数（每包第 0 帧丢弃，剩余帧补零到 NFFT） */
#define DOA_NFFT            1024
#define DOA_LOG2_NFFT       10

/* ---------------- 阵列几何 ---------------- */
/* 外圈 6 个麦克风所在圆的半径（米）。Sipeed 标准 D80 阵列为 40mm（D107 版为 53.5mm），可用卡尺复核 */
#define DOA_ARRAY_RADIUS_M  0.040f
/* 声速（米/秒），20℃ 约 343 */
#define DOA_SOUND_SPEED     343.0f

/*
 * 外圈第 k 个麦克风（k=0..5）的方位角 = DOA_MIC0_ANGLE_DEG + k * 60 * DOA_MIC_DIR
 * DOA_MIC_DIR = +1 表示麦克风编号逆时针递增，-1 表示顺时针。
 * 默认值取自 Sipeed USB 驱动板（BL616）文档中的算法通道角度：
 *   ch0..ch5 = 90°, 30°, 330°, 270°, 210°, 150°（即 0 号麦在 90°，顺时针每 60° 一个）
 * Sipeed 注明该顺序与板上丝印 U1~U6 的对应、以及物理 0° 参考方向尚未验证，上板后仍需标定：
 *   在已知方向放声源，若输出角度与实际相差固定值，调整 DOA_MIC0_ANGLE_DEG；
 *   若声源顺时针移动而输出逆时针变化，把 DOA_MIC_DIR 取反。
 */
#define DOA_MIC0_ANGLE_DEG  0.0f    /* 实测标定（2026-10-17）：0° 定义为指向 0 号麦，逆时针为正 */
#define DOA_MIC_DIR         (+1)    /* 实测：ch0~ch5 沿逆时针依次排列 */

/*
 * 外圈麦克风 k 与中心麦克风对应的 PL 通道号（0..7）。
 * PL 通道：ch = 2*数据线号 + (0:左声道, 1:右声道)
 *   MIC_D0 -> ch0/ch1，MIC_D1 -> ch2/ch3，MIC_D2 -> ch4/ch5，MIC_D3 -> ch6/ch7
 * 每对麦克风哪个是左声道取决于阵列板上 L/R 引脚接法；
 * 上电后串口会打印各通道电平，没信号的那个是中心麦的空声道。
 */
#define DOA_CH_OUTER        { 0, 1, 2, 3, 4, 5 }
#define DOA_CH_CENTER       7       /* 实测：MIC_D3 的左声道 ch6 为空，中心麦在右声道 ch7 */
/* 是否使用中心麦克风参与计算 */
#define DOA_USE_CENTER      1

/* ---------------- 算法 ---------------- */
/* 参与 GCC-PHAT 的频带（Hz） */
#define DOA_FREQ_LO_HZ      300.0f
#define DOA_FREQ_HI_HZ      6000.0f
/* 互相关时延搜索精度：1/DOA_LAG_UPSAMPLE 个采样点 */
#define DOA_LAG_UPSAMPLE    4
/* 方位角搜索步进（度） */
#define DOA_AZ_STEP_DEG     1

/*
 * 声音检测：当前块频带能量比背景噪声高出 DOA_DETECT_DB 才输出角度。
 * 背景噪声 = 最近 DOA_NOISE_WIN_BLOCKS 块（约 5 秒）频带能量的最小值；
 * 持续声音（警笛）期间由识别模块冻结噪声估计。
 */
#define DOA_DETECT_DB       8.0f
#define DOA_NOISE_WIN_BLOCKS 240
/* SRP 峰值相对平均值的最小比值，太小说明方向不明确（混响/多声源） */
#define DOA_MIN_PEAK_RATIO  3.0f    /* 实测：1.5 时约 40% 输出为反射等离群值，3.0 时正确率约 92% */
/*
 * 频率掩码：只用功率高于该频点背景噪声 DOA_MASK_DB 的频点做 GCC-PHAT，
 * 有效频点少于 DOA_MASK_MIN_BINS 时退回全频带。
 */
#define DOA_MASK_DB         10.0f
#define DOA_MASK_MIN_BINS   6

/*
 * 目标频率聚焦定位（doa_focus_at / siren_focus）：锁定警笛频率后只用它的谐波频点定位，
 * 抑制其他声源把方向拉走。谱峰至少比 300~2500Hz 中位数高 MIN_PROM_DB；
 * 1/2、1/3 处高 SUB_PROM_DB 以上视为基频；每个谐波取 ±HALF_BINS 个频点，总数至少 MIN_BINS。
 */
#ifndef DOA_FOCUS_ENABLE
#define DOA_FOCUS_ENABLE            1
#endif
#define DOA_FOCUS_MIN_PROM_DB       9.0f
#ifndef DOA_FOCUS_SUB_PROM_DB
#define DOA_FOCUS_SUB_PROM_DB       6.0f
#endif
/* 聚焦定位使用的最高频率：太高的分量在 8cm 阵列上有空间混叠（方向出现"鬼峰"） */
#ifndef DOA_FOCUS_MAX_HZ
#define DOA_FOCUS_MAX_HZ            DOA_FREQ_HI_HZ
#endif
#define DOA_FOCUS_HALF_BINS         2
#define DOA_FOCUS_MAX_BINS          64
#ifndef DOA_FOCUS_MIN_BINS
#define DOA_FOCUS_MIN_BINS          2
#endif
/*
 * 持续噪声互功率谱减法（doa.c noise_xspec_update）：频点不高于两侧邻近频点（±3~±NX_NEIGH）平均 NX_GATE_DB
 * 时视为纯噪声，按 NX_ALPHA（约 0.4s）更新平均；至少 NX_MIN_N 次更新后才在聚焦定位中减去。
 */
/* 谱峰信噪比的背景：各频点 dB 域指数平均，每块权重 BG_ALPHA（约 5s） */
#define DOA_BG_ALPHA                (1.0f / 240.0f)
#ifndef DOA_NX_SUBTRACT
#define DOA_NX_SUBTRACT             0       /* 试过：对音乐等非平稳干扰会引入偏差，默认只用频点筛选 */
#endif
#ifndef DOA_NX_SELECT
#define DOA_NX_SELECT               1
#endif
#define DOA_NX_NEIGH                8
#define DOA_NX_GATE_DB              6.0f
#define DOA_NX_ALPHA                0.05f
#define DOA_NX_MIN_N                10
#define DOA_NX_SEL_DB               6.0f    /* 聚焦频点须高于该频点平均噪声功率的 dB 数 */
/*
 * 警笛方向槽（siren_focus.c）：每块最多聚焦定位 MAX_FOCUS 个音调分量，按方位归入 ±DIR_CLUSTER_DEG 内的槽
 * （最多 SLOTS 个方向），每槽统计最近 WIN 块，至少 MIN_N 块后才参与判定。
 * 排序加分：与槽预测频率相差 <= MAX_STEP_HZ 时加 SLOT_BONUS_DB（目标槽 TARGET_BONUS_DB），
 * 每 Hz 偏差扣 PENALTY_DB_PER_HZ；跳回该槽 MODE_MIN_AGE 块前用过的频率 ±MODE_TOL_HZ 内（两音交替）扣 MODE_JUMP_COST_DB。
 * 槽方向每次观测向新方位移动 DIR_FOLLOW。
 */
#define SF_SLOTS                    4
#define SF_WIN                      48      /* = SC_SIREN_WIN，约 1 秒 */
#define SF_MIN_N                    32
#define SF_MAX_FOCUS                4
#define SF_MAX_STEP_HZ              60.0f
#define SF_PENALTY_DB_PER_HZ        0.25f
#define SF_SLOT_BONUS_DB            4.0f
#define SF_TARGET_BONUS_DB          12.0f
#define SF_MODE_MIN_AGE             10
#define SF_MODE_TOL_HZ              25.0f
#define SF_MODE_JUMP_COST_DB        3.0f
#define SF_STRICT_FRAC              0.6f    /* 严格目标方向的音调块占比（只选方向，识别门限见 SC_SIREN_ON_FRAC） */
#define SF_REANCHOR_N               2       /* 连续几个彼此连续的离群点才认为频率真的跳了 */
#define SF_MAX_JUMPS                1       /* 1 秒内允许的无法解释跳变（两音警笛第一次换音） */
/* 宽带已确认警笛、但没有严格符合的方向时的备用目标门限 */
#ifndef SF_FALLBACK_FRAC
#define SF_FALLBACK_FRAC            0.3f
#endif
#define SF_FALLBACK_MIN_SCORE       0.15f   /* 备用目标最低得分（见 siren_focus.c fallback_score） */
#define SF_FALLBACK_SWITCH          0.15f   /* 已有备用目标时，新方向得分须高出这么多才换 */
/* 警笛仍被确认时，严格目标方向的音调块占比降到此值以下才放弃（干扰盖住警笛时的滞回） */
#define SF_HOLD_FRAC                0.1f
#ifndef SF_DIR_CLUSTER_DEG
#define SF_DIR_CLUSTER_DEG          25.0f
#endif
#define SF_DIR_FOLLOW               0.1f


/* ---------------- 声音识别（拍手 / 救护车警笛） ---------------- */
/* Default deployment is siren-only. This rule detector is not yet an
 * ambulance-vs-police/fire classifier. Legacy regression uses -DSC_TARGET_ONLY=0. */
#ifndef SC_TARGET_ONLY
#define SC_TARGET_ONLY          1
#endif
#define TARGET_DIR_CONFIRM_BLOCKS 3
#define TARGET_DIR_FRESH_BLOCKS   48  /* 约 1s 没有新的方向证据则显示 UNK（原 0.25s，实录中频繁闪 UNK） */
#define TARGET_DIR_CLUSTER_DEG    20.0f
/*
 * 显示稳定：房间混响中警笛（窄带）每块方向散布约 ±20°，直接显示会一直跳。
 * 显示值 = 方向的长时圆周平均（每个有效块权重 DISPLAY_ALPHA，约 1~2s），
 * 只有偏离当前显示超过 DISPLAY_DEADBAND_DEG 才更新，并取整到 DISPLAY_STEP_DEG。
 * 方向滤波确认了大跳变（声源真的换了位置）或重新确认方向时，平均值直接从新方向开始。
 */
#define TARGET_DISPLAY_ALPHA        0.08f
#define TARGET_DISPLAY_DEADBAND_DEG 3.0f
#define TARGET_DISPLAY_STEP_DEG     1.0f
/*
 * 拍手（子帧级检测，见 sound_class.c）：短时能量比 3~20ms 前突增 RISE_DB，
 * 20ms 内峰值比背景高 MIN_SNR_DB，峰值后约 100ms 衰减 DECAY_DB，峰均比（±50ms）>= MIN_CREST_DB，
 * 峰值所在块宽带（平坦度、质心）；确认后 REFRACT_MS 内不再触发。
 * 实测（1 米拍手）：峰值高于背景 22~25dB，100ms 衰减 18~19dB，峰均比 11~13dB，平坦度 0.46~0.58
 */
#define SC_CLAP_RISE_DB         10.0f
#define SC_CLAP_MIN_SNR_DB      12.0f
#define SC_CLAP_DECAY_DB        10.0f
#define SC_CLAP_MIN_CREST_DB    8.0f
#define SC_CLAP_MIN_FLATNESS    0.10f
#define SC_CLAP_MIN_CENTROID_HZ 1000.0f
#define SC_CLAP_REFRACT_MS      150.0f
/* 拍手方向门限（直达声为主；手在阵列上方时峰值比偏低，实测 2.0~4.0） */
#define SC_CLAP_MIN_PEAK_RATIO  2.0f
/*
 * 警笛：在最近 SC_SIREN_WIN 块（约 1 秒）中，"音调块"（最强谱峰在 SIREN_F_LO~F_HI 之间、
 * 比中位数高 SIREN_TONAL_DB、信噪比 >= SIREN_MIN_SNR_DB）占比 >= ON_FRAC，
 * 且谱峰频率有明显起伏（10%~90% 分位差 >= SIREN_MIN_SPAN_HZ：两音交替或连续升降）。
 * 占比降到 OFF_FRAC 以下时结束。
 */
#define SC_SIREN_WIN            48
#define SC_SIREN_F_LO_HZ        400.0f
#define SC_SIREN_F_HI_HZ        2500.0f /* 2026-10-19实录主峰为1844/2373Hz强三次谐波，旧2000Hz排除了高音段 */
#ifndef SC_SIREN_TONAL_DB
#define SC_SIREN_TONAL_DB       9.0f
#endif
#define SC_SIREN_MIN_SNR_DB     8.0f
/* 开始条件须连续满足的块数（约 0.75s）：实录人声误报段最长连续 16 块，警笛（含噪声/干扰下）>= 113 块 */
#define SC_SIREN_CONFIRM_BLOCKS 36
/* 开始条件：1 秒内相邻音调块谱峰"无规律跳变"（差 > FLIP_HZ 且非整数倍）的比例 <= MAX_FLIP。
 * 合成长喇叭 0.17~0.48、实录人声 0.16~0.36；警笛中位数 <= 0.06、实录警笛 90% <= 0.15 */
#define SC_SIREN_FLIP_HZ        60.0f
#define SC_SIREN_MAX_FLIP       0.2f
#define SC_SIREN_ON_FRAC        0.7f    /* 2026-10-24 由 0.6 提高：实录安静房间误报段 1 秒内音调块最多 0.52 */
#define SC_SIREN_OFF_FRAC       0.3f
#define SC_SIREN_MIN_SPAN_HZ    100.0f
/* 开始判定时音调块能量的 10%~90% 范围上限（实测警笛中位 3~6dB；拨弦类音乐每个音衰减，>= 9dB） */
#define SC_SIREN_MAX_ERANGE_DB  8.0f
/* A previously confirmed siren must not remain active indefinitely after
 * changing into a steady horn/whistle. Allow about 2s of pattern degradation. */
#define SC_SIREN_PATTERN_HOLD_BLOCKS 96
/* 警笛块的方向有效门限（窄带声音的 SRP 峰较宽，峰值比天然偏低） */
#define SC_SIREN_MIN_PEAK_RATIO 2.0f

/* ---------------- 远近等级（暂定阈值，需房间实测标定） ---------------- */
/* 相干度仅是直达声/混响的粗略线索，不能对应固定米数。
 * 所有类别共用；短促拍手证据不足时通常 UNKNOWN。 */
/*
 * 目标模式（警笛）按音量估距离：RANGE_REF_LEVEL_DB 为 RANGE_REF_DIST_M 处的警笛谐波功率（dB，相对值），
 * 上电默认值来自 2026-11-01 现场实测：喇叭在 40cm 时音量中位数 2.4dB，按距离反比折算到 1 米（1 米处未实测）；
 * 换喇叭或音量后要在 1 米处放警笛、串口发 'C' 标定。
 * 档位：< RANGE_NEAR_M 近，> RANGE_FAR_M 远；用最近 RANGE_LEVEL_WIN 个有效块（至少 RANGE_LEVEL_MIN_N）的中位数。
 */
#ifndef RANGE_REF_LEVEL_DB
#define RANGE_REF_LEVEL_DB       (-5.6f)
#endif
#define RANGE_REF_DIST_M         1.0f
/* 2026-11-01 用户要求约 1 米显示"远"、40cm 稳定显示"中"。滞回 15%：约 0.75m 变远、低于 0.57m 才退回中；低于 0.22m 变近；房间混响使远处估计偏小，所以 FAR 取得比 1 米小 */
#define RANGE_NEAR_M             0.25f
#define RANGE_FAR_M              0.65f
#define RANGE_LEVEL_WIN          144     /* 约 3 秒：实测同一位置 1 秒窗口的中位数随扫频摆动约 5dB，档位来回跳 */
#define RANGE_LEVEL_MIN_N        12
#define RANGE_COH_NEAR           0.55f
#define RANGE_COH_FAR            0.30f
#define RANGE_WINDOW_BLOCKS      8       /* 约 168ms */
#define RANGE_MIN_VALID_BLOCKS   5
#define RANGE_MIN_SNR_DB         8.0f
#define RANGE_MAX_SPREAD         0.15f
#define RANGE_BOUNDARY_MARGIN    0.04f

/* ---------------- 调试 ---------------- */
/* 调试录音缓冲区块数（每块约 21ms、32KB）：768 块约 16 秒、25MB DDR；设为 0 关闭。
 * 模式见 main.c（只录响亮块 / 待触发连续录音） */
#define DEBUG_RECORD_BLOCKS     768
#define DEBUG_RECORD_PREROLL    48      /* 连续录音模式下保留触发前的块数（约 1 秒） */
#define DEBUG_RECORD_MIN_SNR_DB 12.0f

/* ---------------- SK9822 灯环 ---------------- */
#define LED_NUM             12
/* 0 号 LED 所在方位角（度），以及 LED 编号递增方向（+1 逆时针，-1 顺时针） */
#define LED0_ANGLE_DEG      0.0f
#define LED_DIR             (+1)
/* 亮度 0..31 */
#define LED_BRIGHTNESS      4

#endif
