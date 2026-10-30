#include "sound_class.h"
#include "doa_config.h"
#include <math.h>

#define CLAP_HOLD_BLOCKS    72      /* 拍手结果保持显示的块数（约 1.5s） */
#define OTHER_HOLD_BLOCKS   48

/*
 * 拍手检测（子帧级）。拍手只有几毫秒，用 21ms 的块来判断太粗：块边缘的拍手会被加窗削弱，
 * 拍手的回声、"啪啪"两下也会在相邻块里反复触发。因此在 128 点（约 2.6ms）、
 * 步进 32 点（约 0.66ms）的短时能量序列 L 上检测：
 *   1. 起点：L 比 3~20ms 前的最大值突增 SC_CLAP_RISE_DB
 *   2. 起点后 20ms 内的峰值比背景高 SC_CLAP_MIN_SNR_DB
 *   3. 峰值后约 100ms 已衰减 SC_CLAP_DECAY_DB（短促）
 *   4. 峰值相对前后 ±50ms 平均能量的峰均比 >= SC_CLAP_MIN_CREST_DB（能量集中）
 *   5. 峰值所在块宽带（平坦度、质心）
 *   6. 确认后 SC_CLAP_REFRACT_MS 内不再触发（一次拍手的回声和"啪啪"只算一次）
 * 因为要看峰值后约 110ms，检测有约 0.13 秒延迟。
 */
#define SUB_MS              (32.0f * 1000.0f / DOA_FS_HZ)       /* 每个子帧的毫秒数 */
#define MS2SUB(ms)          ((uint32_t)((ms) / SUB_MS + 0.5f))
#define CL_RING             1024                                /* 子帧环形缓冲（约 0.67s） */
#define CL_BACK_NEAR        MS2SUB(3.0f)
#define CL_BACK_FAR         MS2SUB(20.0f)
#define CL_PEAK_WIN         MS2SUB(20.0f)
#define CL_DECAY_AT         MS2SUB(100.0f)
#define CL_DECAY_LEN        MS2SUB(10.0f)
#define CL_CREST_HALF       MS2SUB(50.0f)
#define CL_INFO             16                                  /* 记住最近 16 块的方向与谱特征 */

static float    cl_w[CL_RING];          /* 128 点窗能量（线性） */
static float    cl_l[CL_RING];          /* 同上，dB */
static uint32_t cl_n;                   /* 已写入的子帧总数 */
static uint32_t cl_scan;                /* 下一个待检查的子帧序号 */
static uint32_t cl_ready;               /* 不应期结束的子帧序号 */
static float    cl_last3[3];            /* 上一块最后 3 个子帧能量（滑窗跨块） */
static struct { float az, pr, flat, cent; int band; } cl_info[CL_INFO];
static float    pl_hist[DOA_NOISE_WIN_BLOCKS];      /* 每块平均短时能量（dB）的背景：最小值统计 */
static int      pl_n, pl_pos;

/* 警笛检测：最近 SC_SIREN_WIN 块的音调标志与谱峰频率 */
static unsigned char tone_flag[SC_SIREN_WIN];
static float    tone_hz[SC_SIREN_WIN];
static float    tone_e[SC_SIREN_WIN];
/* 相邻音调块（间隔 <= 2 块）的谱峰比较：pair = 有可比的前一块，flip = 频率无规律地跳了
 * （差 > SC_SIREN_FLIP_HZ 且不是整数倍关系） */
static unsigned char tone_pair[SC_SIREN_WIN], tone_flip[SC_SIREN_WIN];
static float    last_tone_hz;
static int      last_tone_age;
static int      win_n, win_pos;
static int      siren_on;
static int      on_run;             /* 开始条件连续满足的块数 */
static int      pattern_bad_blocks;

/* 显示状态 */
static int      cur_cls;
static uint32_t cur_until;
static float    cur_level;
static int      cur_band;

/* 目标模式按音量估距离：最近 RANGE_LEVEL_WIN 个有效块的警笛音量（dB） */
static float lvl_hist[RANGE_LEVEL_WIN];
static int   lvl_n, lvl_pos;
static float range_ref_db = RANGE_REF_LEVEL_DB;
static int   lvl_band;

static float range_coh[RANGE_WINDOW_BLOCKS];
static unsigned char range_ok[RANGE_WINDOW_BLOCKS];
static int range_pos;

void sc_init(void)
{
    int i;
    cl_n = 0;
    cl_scan = 0;
    cl_ready = 0;
    for (i = 0; i < 3; i++)
        cl_last3[i] = 0.0f;
    pl_n = 0;
    pl_pos = 0;
    win_n = 0;
    win_pos = 0;
    last_tone_age = 1000;
    siren_on = 0;
    on_run = 0;
    pattern_bad_blocks = 0;
    range_pos = 0;
    for (i = 0; i < RANGE_WINDOW_BLOCKS; i++) range_ok[i] = 0;
    lvl_n = lvl_pos = 0;
    lvl_band = BAND_NONE;
    cur_cls = SND_NONE;
    cur_level = -200.0f;
    cur_until = 0;
    cur_band = BAND_NONE;
}

const char *sc_class_name(int cls)
{
    switch (cls) {
    case SND_CLAP:  return "CLAP";
    case SND_SIREN: return "SIREN";
    case SND_OTHER: return "SOUND";
    default:        return "";
    }
}

const char *sc_band_name(int band)
{
    switch (band) {
    case BAND_NEAR: return "NEAR";
    case BAND_MID:  return "MID";
    case BAND_FAR:  return "FAR";
    default:        return "UNKNOWN";
    }
}

/* Coherence is a room-dependent proximity cue, not a metric distance.
 * Keep every block (including invalid ones) in a short window so silence
 * cannot preserve a stale range. Boundary / unstable windows are UNKNOWN. */

static int range_update(const doa_result_t *r, int dir_ok)
{
    float lo = 2.0f, hi = -1.0f, sum = 0.0f, mean;
    int i, n = 0;
    int ok = dir_ok && isfinite(r->coherence) &&
             r->coherence >= 0.0f && r->coherence <= 1.0f &&
             (r->energy_db - r->noise_db >= RANGE_MIN_SNR_DB || r->peak_snr_db >= RANGE_MIN_SNR_DB);
    range_coh[range_pos] = r->coherence;
    range_ok[range_pos] = (unsigned char)ok;
    range_pos = (range_pos + 1) % RANGE_WINDOW_BLOCKS;
    for (i = 0; i < RANGE_WINDOW_BLOCKS; i++) {
        if (!range_ok[i]) continue;
        if (range_coh[i] < lo) lo = range_coh[i];
        if (range_coh[i] > hi) hi = range_coh[i];
        sum += range_coh[i];
        n++;
    }
    if (!ok || n < RANGE_MIN_VALID_BLOCKS || hi - lo > RANGE_MAX_SPREAD)
        return BAND_NONE;
    mean = sum / n;
    if (mean > RANGE_COH_NEAR + RANGE_BOUNDARY_MARGIN) return BAND_NEAR;
    if (mean < RANGE_COH_FAR - RANGE_BOUNDARY_MARGIN) return BAND_FAR;
    if (mean > RANGE_COH_FAR + RANGE_BOUNDARY_MARGIN &&
        mean < RANGE_COH_NEAR - RANGE_BOUNDARY_MARGIN) return BAND_MID;
    return BAND_NONE;
}

static float level_median(void)
{
    float t[RANGE_LEVEL_WIN];
    int i, j;
    for (i = 0; i < lvl_n; i++) {           /* 插入排序 */
        float v = lvl_hist[i];
        for (j = i - 1; j >= 0 && t[j] > v; j--)
            t[j + 1] = t[j];
        t[j + 1] = v;
    }
    return t[lvl_n / 2];
}

/*
 * 目标模式的远近：警笛自身的音量（只用警笛各谐波频点的功率，不受别的声源影响），
 * 距离每加倍约小 6dB：d = 10^((参考 - 音量)/20) 米，参考为 1 米处的音量（可用 sc_range_calibrate 标定）。
 * 用最近约 1 秒的中位数（升降调扫频时喇叭/房间频响起伏大），档位切换加 15% 滞回。
 * 房间里超过混响半径（约 1~2 米）后音量随距离下降变慢，远处的距离会偏小。
 */
static int range_level_update(const doa_result_t *r, int dir_ok, float *dist)
{
    float d;
    if (dir_ok && r->target_level_db > -150.0f) {
        lvl_hist[lvl_pos] = r->target_level_db;
        lvl_pos = (lvl_pos + 1) % RANGE_LEVEL_WIN;
        if (lvl_n < RANGE_LEVEL_WIN)
            lvl_n++;
    }
    *dist = -1.0f;
    if (lvl_n < RANGE_LEVEL_MIN_N) {
        lvl_band = BAND_NONE;
        return BAND_NONE;
    }
    d = RANGE_REF_DIST_M * powf(10.0f, (range_ref_db - level_median()) / 20.0f);
    *dist = d;
    if (lvl_band == BAND_NEAR && d < RANGE_NEAR_M * 1.15f)
        return lvl_band;
    if (lvl_band == BAND_FAR && d > RANGE_FAR_M / 1.15f)
        return lvl_band;
    if (lvl_band == BAND_MID && d > RANGE_NEAR_M / 1.15f && d < RANGE_FAR_M * 1.15f)
        return lvl_band;
    lvl_band = d < RANGE_NEAR_M ? BAND_NEAR : d > RANGE_FAR_M ? BAND_FAR : BAND_MID;
    return lvl_band;
}

int sc_range_calibrate(float dist_m)
{
    if (!siren_on || lvl_n < RANGE_LEVEL_MIN_N || dist_m <= 0.0f)
        return 0;
    range_ref_db = level_median() + 20.0f * log10f(dist_m / RANGE_REF_DIST_M);
    lvl_band = BAND_NONE;
    return 1;
}

float sc_range_ref_db(void)
{
    return range_ref_db;
}

static void sort_f(float *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {               /* 插入排序，n <= 48 */
        float t = a[i];
        for (j = i - 1; j >= 0 && a[j] > t; j--)
            a[j + 1] = a[j];
        a[j + 1] = t;
    }
}

/* 警笛窗口统计：音调块占比、谱峰频率与能量的 10%~90% 分位差、音调块平均能量 */
/* f 与 g 是否约成整数倍（同一个声音的基频与谐波之间换） */
static int harmonic_pair(float f, float g)
{
    float r, m;
    if (f <= 0.0f || g <= 0.0f)
        return 0;
    r = f > g ? f / g : g / f;
    m = floorf(r + 0.5f);
    return m >= 2.0f && fabsf(r - m) <= 0.04f * m;
}

static void siren_stats(float *frac, float *span, float *erange, float *level)
{
    float hz[SC_SIREN_WIN], e[SC_SIREN_WIN];
    int i, n = 0;
    double esum = 0.0;

    for (i = 0; i < win_n; i++) {
        if (tone_flag[i]) {
            hz[n] = tone_hz[i];
            e[n] = tone_e[i];
            esum += tone_e[i];
            n++;
        }
    }
    *frac = win_n ? (float)n / (float)win_n : 0.0f;
    *level = n ? (float)(esum / n) : -200.0f;
    *span = 0.0f;
    *erange = 100.0f;
    if (n < 4)
        return;
    sort_f(hz, n);
    sort_f(e, n);
    *span = hz[(n * 9) / 10] - hz[n / 10];
    *erange = e[(n * 9) / 10] - e[n / 10];
}

#define CLW(k)  cl_w[(k) % CL_RING]
#define CLL(k)  cl_l[(k) % CL_RING]

/* 写入本块 32 个子帧，并记录本块方向与谱特征；返回本块平均短时能量（dB） */
static float clap_push(const doa_result_t *r, int band)
{
    uint32_t c = cl_n / DOA_NSUB;
    double sum = 0.0;
    int j;

    cl_info[c % CL_INFO].band = band;
    cl_info[c % CL_INFO].az = r->azimuth_deg;
    cl_info[c % CL_INFO].pr = r->peak_ratio;
    cl_info[c % CL_INFO].flat = r->flatness;
    cl_info[c % CL_INFO].cent = r->centroid_hz;
    for (j = 0; j < DOA_NSUB; j++) {
        float w = (r->sub_e[j] + cl_last3[0] + cl_last3[1] + cl_last3[2]) / 128.0f;
        cl_last3[0] = cl_last3[1];
        cl_last3[1] = cl_last3[2];
        cl_last3[2] = r->sub_e[j];
        CLW(cl_n) = w;
        CLL(cl_n) = 10.0f * log10f(w + 1e-20f);
        sum += w;
        cl_n++;
    }
    return 10.0f * log10f((float)(sum / DOA_NSUB) + 1e-20f);
}

/* 扫描已有足够"后文"的子帧，找拍手；找到返回 1 并给出峰值电平与方位 */
static int clap_scan(float floor_db, float *level, float *az, int *band)
{
    while (cl_scan + CL_PEAK_WIN + CL_DECAY_AT + CL_DECAY_LEN < cl_n) {
        uint32_t j = cl_scan, k, pk;
        float back = -200.0f, lp, tail = -200.0f, crest;
        double wsum = 0.0;

        if (j < cl_ready || j < CL_BACK_FAR + CL_CREST_HALF ||
            cl_n - j + CL_BACK_FAR + CL_CREST_HALF > CL_RING) {
            cl_scan++;
            continue;
        }
        for (k = j - CL_BACK_FAR; k <= j - CL_BACK_NEAR; k++)
            if (CLL(k) > back)
                back = CLL(k);
        if (CLL(j) - back < SC_CLAP_RISE_DB) {
            cl_scan++;
            continue;
        }
        pk = j;
        for (k = j; k < j + CL_PEAK_WIN; k++)
            if (CLL(k) > CLL(pk))
                pk = k;
        lp = CLL(pk);
        cl_scan = pk + 1;                    /* 无论是否确认，都从峰值之后继续 */
        if (lp - floor_db < SC_CLAP_MIN_SNR_DB)
            continue;
        for (k = pk + CL_DECAY_AT; k < pk + CL_DECAY_AT + CL_DECAY_LEN; k++)
            if (CLL(k) > tail)
                tail = CLL(k);
        for (k = pk - CL_CREST_HALF; k <= pk + CL_CREST_HALF; k++)
            wsum += CLW(k);
        crest = lp - 10.0f * log10f((float)(wsum / (2 * CL_CREST_HALF + 1)) + 1e-20f);
        {
            uint32_t c = pk / DOA_NSUB, cb = c, i;
            /* 峰值所在块及其前后块中，方向最明确的一块作为拍手方向 */
            for (i = (c > 0 ? c - 1 : c); i <= c + 1 && i < cl_n / DOA_NSUB; i++)
                if (cl_info[i % CL_INFO].pr > cl_info[cb % CL_INFO].pr)
                    cb = i;
            if (lp - tail >= SC_CLAP_DECAY_DB && crest >= SC_CLAP_MIN_CREST_DB &&
                cl_info[c % CL_INFO].flat >= SC_CLAP_MIN_FLATNESS &&
                cl_info[c % CL_INFO].cent >= SC_CLAP_MIN_CENTROID_HZ) {
                cl_ready = pk + MS2SUB(SC_CLAP_REFRACT_MS);
                *band = cl_info[cb % CL_INFO].band;
                *level = lp;
                *az = cl_info[cb % CL_INFO].pr >= SC_CLAP_MIN_PEAK_RATIO ? cl_info[cb % CL_INFO].az : -1.0f;
                return 1;
            }
        }
    }
    return 0;
}

void sc_process(const doa_result_t *r, uint32_t blk, snd_state_t *out)
{
    float snr = r->energy_db - r->noise_db;
    /* 警笛用"整体信噪比或谱峰信噪比"：持续宽带噪声（风扇、人声嘈杂）把整体信噪比压到 0 附近时，
     * 警笛的谱峰仍高出同频点背景很多 */
    float tone_snr = r->peak_snr_db > snr ? r->peak_snr_db : snr;
    int tonal, range_band;
    float frac, span, erange, slevel, flip;

    out->event = 0;
    out->event_az = -1.0f;

    /* ---------- 警笛 ---------- */
    tonal = r->tonality_db >= SC_SIREN_TONAL_DB &&
            r->peak_hz >= SC_SIREN_F_LO_HZ && r->peak_hz <= SC_SIREN_F_HI_HZ &&
            tone_snr >= SC_SIREN_MIN_SNR_DB;
    tone_flag[win_pos] = (unsigned char)tonal;
    tone_pair[win_pos] = tone_flip[win_pos] = 0;
    if (tonal) {
        if (last_tone_age <= 2) {
            tone_pair[win_pos] = 1;
            tone_flip[win_pos] = (unsigned char)(fabsf(r->peak_hz - last_tone_hz) > SC_SIREN_FLIP_HZ &&
                                                 !harmonic_pair(r->peak_hz, last_tone_hz));
        }
        last_tone_hz = r->peak_hz;
        last_tone_age = 0;
    }
    last_tone_age++;
    tone_hz[win_pos] = r->peak_hz;
    tone_e[win_pos] = r->energy_db;
    win_pos = (win_pos + 1) % SC_SIREN_WIN;
    if (win_n < SC_SIREN_WIN)
        win_n++;
    siren_stats(&frac, &span, &erange, &slevel);
    {
        int i, np = 0, nf = 0;
        for (i = 0; i < win_n; i++) {
            np += tone_pair[i];
            nf += tone_flip[i];
        }
        flip = np ? (float)nf / (float)np : 0.0f;
    }

    /*
     * 开始条件还要求响度平稳：警笛持续发声，音乐的每个音都会衰减。
     * 并且要连续满足 SC_SIREN_CONFIRM_BLOCKS 块才确认：警笛一响就是好几秒，
     * 人说话、哼唱偶尔凑够条件也只能维持零点几秒（实录误报段最长 16 块）。
     * 还要求谱峰不乱跳：警笛的两个音是轮流响的，一个音上停留零点几秒；
     * 汽车喇叭的两个音、人声与发动机的谐波是同时存在的，最强谱峰几乎每块都在它们之间跳。
     */
    if (!siren_on) {
        if (win_n == SC_SIREN_WIN && frac >= SC_SIREN_ON_FRAC &&
            span >= SC_SIREN_MIN_SPAN_HZ && erange <= SC_SIREN_MAX_ERANGE_DB &&
            flip <= SC_SIREN_MAX_FLIP)
            on_run++;
        else
            on_run = 0;
        if (on_run >= SC_SIREN_CONFIRM_BLOCKS) {
            siren_on = 1;
            on_run = 0;
            pattern_bad_blocks = 0;
            out->event = 1;
        }
    } else if (siren_on && frac < SC_SIREN_OFF_FRAC) {
        siren_on = 0;
        pattern_bad_blocks = 0;
    } else if (siren_on) {
        /* Energy stability gates initial confirmation only. A real siren may
         * fade with distance or playback envelope while retaining its pattern. */
        if (span < SC_SIREN_MIN_SPAN_HZ) {
            if (++pattern_bad_blocks >= SC_SIREN_PATTERN_HOLD_BLOCKS)
                siren_on = 0;
        } else {
            pattern_bad_blocks = 0;
        }
    }
    out->siren_active = siren_on;
    out->dir_ok = r->valid ||
                  (siren_on && tonal && r->peak_ratio >= SC_SIREN_MIN_PEAK_RATIO);
    if (SC_TARGET_ONLY)
        out->dir_ok = siren_on && tonal && r->peak_ratio >= SC_SIREN_MIN_PEAK_RATIO;

    out->distance_m = -1.0f;
    if (SC_TARGET_ONLY) {
        if (!siren_on) {
            lvl_n = lvl_pos = 0;            /* 每次警笛重新累计，不沿用上一次的音量 */
            lvl_band = BAND_NONE;
        }
        range_band = range_level_update(r, out->dir_ok, &out->distance_m);
    } else {
        range_band = range_update(r, out->dir_ok);
    }

    /* ---------- 拍手 ---------- */
    if (!SC_TARGET_ONLY) {
        float blk_db = clap_push(r, range_band), floor_db = blk_db, level, az;
        int event_band;
        int i;
        for (i = 0; i < pl_n; i++)
            if (pl_hist[i] < floor_db)
                floor_db = pl_hist[i];
        if (!siren_on) {
            pl_hist[pl_pos] = blk_db;
            pl_pos = (pl_pos + 1) % DOA_NOISE_WIN_BLOCKS;
            if (pl_n < DOA_NOISE_WIN_BLOCKS)
                pl_n++;
        }
        if (clap_scan(floor_db, &level, &az, &event_band) && !siren_on) {
            out->event = 1;
            out->event_az = az;
            cur_cls = SND_CLAP;
            cur_until = blk + CLAP_HOLD_BLOCKS;
            cur_level = level;
            cur_band = az >= 0.0f ? event_band : BAND_NONE;
        }
    }

    /* ---------- 当前显示类别与远近等级 ---------- */
    if (siren_on) {
        cur_cls = SND_SIREN;
        cur_until = blk + OTHER_HOLD_BLOCKS;
        cur_level = slevel;
        cur_band = range_band;
    } else if (SC_TARGET_ONLY) {
        cur_cls = SND_NONE;
        cur_band = BAND_NONE;
    } else if (r->valid && !(cur_cls == SND_CLAP && blk < cur_until)) {
        cur_cls = SND_OTHER;
        cur_until = blk + OTHER_HOLD_BLOCKS;
        cur_level = r->energy_db;
        cur_band = range_band;
    } else if (cur_cls != SND_NONE && (int32_t)(blk - cur_until) > 0) {
        cur_cls = SND_NONE;
        cur_band = BAND_NONE;
    }

    /* A held class label must not preserve a stale continuous-source range.
     * Clap holds its historical event result rather than the later background. */
    if (!out->dir_ok && cur_cls != SND_CLAP && !SC_TARGET_ONLY)
        cur_band = BAND_NONE;
    out->cls = cur_cls;
    out->level_db = cur_level;
    out->band = cur_band;
}
