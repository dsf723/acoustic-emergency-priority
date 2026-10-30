/*
 * 声源方位估计：GCC-PHAT + SRP（Steered Response Power），并提取声音识别用的频谱特征
 *
 * 1. 每个麦克风去直流、加 Hann 窗、做 NFFT 点 FFT
 * 2. 频率掩码：只使用功率明显高于该频点背景噪声的频点（窄带声音如警笛只占少数频点，
 *    若对所有频点等权的 PHAT 加权，大部分权重会落在只有噪声的频点上）
 * 3. 对每一对麦克风计算 PHAT 加权互功率谱 G_ij = X_i X_j* / |X_i X_j|（只取掩码内频点）
 * 4. 在 ±MAXLAG 采样点范围内、以 1/UPSAMPLE 采样点为步进计算互相关 r_ij(tau)
 * 5. 对每个候选方位角 theta，按远场模型算出每对麦克风的理论时延 tau_ij(theta)，
 *    SRP(theta) = sum_ij r_ij(tau_ij(theta))（线性插值），取最大者并抛物线插值细化
 *
 * 时延约定：x_i(t) = s(t - t_i)，r_ij 的峰值位于 tau = t_i - t_j；
 *           声源方向单位向量 u，t_m = -(p_m . u)/c，所以 tau_ij = (p_j - p_i) . u / c
 */
#include "doa.h"
#include "doa_config.h"
#include <math.h>
#include <string.h>

#define DOA_PI              3.14159265358979f
#define DOA_NUM_MICS_MAX    7
#define DOA_NUM_PAIRS_MAX   (DOA_NUM_MICS_MAX * (DOA_NUM_MICS_MAX - 1) / 2)
#define DOA_MAX_LAG         32                                  /* 采样点 */
#define DOA_NUM_LAGS_MAX    (2 * DOA_MAX_LAG * DOA_LAG_UPSAMPLE + 1)
#define DOA_NUM_AZ          (360 / DOA_AZ_STEP_DEG)
#define DOA_NBINS_MAX       (DOA_NFFT / 2)

static const int ch_outer[6] = DOA_CH_OUTER;

static int   num_mics;
static int   mic_ch[DOA_NUM_MICS_MAX];
static float mic_x[DOA_NUM_MICS_MAX], mic_y[DOA_NUM_MICS_MAX];     /* 单位：采样点 */

static int   num_pairs;
static int   pair_i[DOA_NUM_PAIRS_MAX], pair_j[DOA_NUM_PAIRS_MAX];
static float pair_dx[DOA_NUM_PAIRS_MAX], pair_dy[DOA_NUM_PAIRS_MAX];

static int   max_lag;        /* 实际搜索范围（采样点） */
static int   num_lags;
static int   k_lo, k_hi;     /* 定位频带对应的 FFT bin */

static float az_cos[DOA_NUM_AZ], az_sin[DOA_NUM_AZ];
static float window[DOA_NFFT];
static float tw_cos[DOA_NFFT / 2], tw_sin[DOA_NFFT / 2];

static float sig_re[DOA_NUM_MICS_MAX][DOA_NFFT];
static float sig_im[DOA_NUM_MICS_MAX][DOA_NFFT];
static float psd[DOA_NBINS_MAX];                 /* 各麦克风平均功率谱 */
static float psd_noise[DOA_NBINS_MAX];           /* 各频点背景噪声（下包络） */
static float psd_bg_db[DOA_NBINS_MAX];           /* 各频点长时平均背景（dB 域指数平均，持续的嗡嗡声也算背景） */
static unsigned char bin_mask[DOA_NBINS_MAX];
static float g_re[DOA_NUM_PAIRS_MAX][DOA_NBINS_MAX];
static float g_im[DOA_NUM_PAIRS_MAX][DOA_NBINS_MAX];
static float xcorr[DOA_NUM_PAIRS_MAX][DOA_NUM_LAGS_MAX];
static float w_re[DOA_NBINS_MAX], w_im[DOA_NBINS_MAX];
static float srp[DOA_NUM_AZ];
static float band_median;                        /* 本块 300~2500Hz 功率谱中位数（tonality 的基准） */
static int   focus_bins[DOA_FOCUS_MAX_BINS];
static float focus_gre[DOA_NUM_PAIRS_MAX][DOA_FOCUS_MAX_BINS];
static float focus_gim[DOA_NUM_PAIRS_MAX][DOA_FOCUS_MAX_BINS];
/* 各麦克风对、各频点的持续噪声互功率谱（只在不含音调成分的块更新），聚焦定位时减去 */
static float nx_re[DOA_NUM_PAIRS_MAX][DOA_NBINS_MAX];
static float nx_im[DOA_NUM_PAIRS_MAX][DOA_NBINS_MAX];
static float nx_psd[DOA_NBINS_MAX];             /* 同样条件下更新的各频点平均噪声功率 */
static unsigned short nx_n[DOA_NBINS_MAX];
static float psd_csum[DOA_NBINS_MAX + 1];
#define DOA_PEAK_SUB        32                          /* 短时能量：32 点子帧累加 */
#define DOA_PEAK_NSUB       4                           /* 4 个子帧滑动求和 = 128 点（约 2.6ms）窗、32 点步进 */
static float frame_e[DOA_NFFT / DOA_PEAK_SUB];

/* 频带能量背景噪声：最近 DOA_NOISE_WIN_BLOCKS 块能量的最小值（最小值统计） */
static float e_hist[DOA_NOISE_WIN_BLOCKS];
static int   e_hist_n, e_hist_pos;
static float noise_db;
static int   noise_freeze;
static int   psd_init;

/* ---------------- FFT（基 2，原位，迭代） ---------------- */
static void fft(float *re, float *im)
{
    int n = DOA_NFFT;
    int i, j, k, len;

    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;
        for (i = 0; i < n; i += len) {
            for (k = 0; k < half; k++) {
                float wr = tw_cos[k * step];
                float wi = -tw_sin[k * step];
                float xr = re[i + k + half] * wr - im[i + k + half] * wi;
                float xi = re[i + k + half] * wi + im[i + k + half] * wr;
                re[i + k + half] = re[i + k] - xr;
                im[i + k + half] = im[i + k] - xi;
                re[i + k] += xr;
                im[i + k] += xi;
            }
        }
    }
}

static int hz2bin(float hz)
{
    int k = (int)(hz * DOA_NFFT / DOA_FS_HZ + 0.5f);
    if (k < 1)
        k = 1;
    if (k > DOA_NFFT / 2 - 1)
        k = DOA_NFFT / 2 - 1;
    return k;
}

void doa_init(void)
{
    int i, j, k;
    float r_smp = DOA_ARRAY_RADIUS_M * DOA_FS_HZ / DOA_SOUND_SPEED;
    float dmax = 0.0f;

    /* 麦克风位置（单位换算成采样点：距离 * fs / c） */
    num_mics = 0;
    for (k = 0; k < 6; k++) {
        float a = (DOA_MIC0_ANGLE_DEG + k * 60.0f * DOA_MIC_DIR) * DOA_PI / 180.0f;
        mic_ch[num_mics] = ch_outer[k];
        mic_x[num_mics]  = r_smp * cosf(a);
        mic_y[num_mics]  = r_smp * sinf(a);
        num_mics++;
    }
#if DOA_USE_CENTER
    mic_ch[num_mics] = DOA_CH_CENTER;
    mic_x[num_mics]  = 0.0f;
    mic_y[num_mics]  = 0.0f;
    num_mics++;
#endif

    num_pairs = 0;
    for (i = 0; i < num_mics; i++) {
        for (j = i + 1; j < num_mics; j++) {
            float dx = mic_x[j] - mic_x[i];
            float dy = mic_y[j] - mic_y[i];
            float d  = sqrtf(dx * dx + dy * dy);
            pair_i[num_pairs]  = i;
            pair_j[num_pairs]  = j;
            pair_dx[num_pairs] = dx;
            pair_dy[num_pairs] = dy;
            if (d > dmax)
                dmax = d;
            num_pairs++;
        }
    }

    max_lag = (int)ceilf(dmax) + 1;
    if (max_lag > DOA_MAX_LAG)
        max_lag = DOA_MAX_LAG;
    num_lags = 2 * max_lag * DOA_LAG_UPSAMPLE + 1;

    k_lo = hz2bin(DOA_FREQ_LO_HZ);
    k_hi = hz2bin(DOA_FREQ_HI_HZ);

    for (i = 0; i < DOA_NUM_AZ; i++) {
        float a = i * DOA_AZ_STEP_DEG * DOA_PI / 180.0f;
        az_cos[i] = cosf(a);
        az_sin[i] = sinf(a);
    }
    for (i = 0; i < DOA_NFFT; i++)
        window[i] = 0.5f - 0.5f * cosf(2.0f * DOA_PI * i / (DOA_NFFT - 1));
    for (i = 0; i < DOA_NFFT / 2; i++) {
        tw_cos[i] = cosf(2.0f * DOA_PI * i / DOA_NFFT);
        tw_sin[i] = sinf(2.0f * DOA_PI * i / DOA_NFFT);
    }

    e_hist_n = 0;
    e_hist_pos = 0;
    noise_db = 0.0f;
    noise_freeze = 0;
    psd_init = 0;
    memset(nx_n, 0, sizeof(nx_n));
}

void doa_noise_freeze(int freeze)
{
    noise_freeze = freeze;
}

/* 拆包：检查通道号与帧序号，转换为浮点。成功返回使用的帧数，失败返回 -1 */
static int unpack(const uint32_t *pkt, int nframes, doa_result_t *res)
{
    double acc[8] = {0};
    int f, c, m, n;
    int seq_ok = 1;
    int seq_prev = -1;

    n = nframes - 1;                  /* 丢弃第 0 帧 */
    if (n > DOA_NFFT)
        n = DOA_NFFT;

    for (f = 0; f < n; f++) {
        const uint32_t *w = &pkt[(f + 1) * 8];
        int seq = (int)(w[0] >> 28);
        if (seq_prev >= 0 && seq != ((seq_prev + 1) & 15))
            seq_ok = 0;
        seq_prev = seq;
        for (c = 0; c < 8; c++) {
            int32_t s = ((int32_t)(w[c] << 8)) >> 8;
            if ((int)((w[c] >> 24) & 7) != c || (int)(w[c] >> 28) != seq)
                seq_ok = 0;
            acc[c] += (double)s * s;
        }
        for (m = 0; m < num_mics; m++) {
            int32_t s = ((int32_t)(w[mic_ch[m]] << 8)) >> 8;
            sig_re[m][f] = (float)s * (1.0f / 8388608.0f);
        }
    }

    for (c = 0; c < 8; c++) {
        double ms = acc[c] / (n > 0 ? n : 1) / (8388608.0 * 8388608.0);
        res->ch_level_db[c] = 10.0f * log10f((float)ms + 1e-20f);
    }
    return seq_ok ? n : -1;
}

/* 背景噪声更新：频带能量用最小值统计，各频点功率用下包络跟踪；冻结时不更新 */
static void update_noise(float energy_db)
{
    int i, k;

    if (!noise_freeze || e_hist_n == 0) {
        e_hist[e_hist_pos] = energy_db;
        e_hist_pos = (e_hist_pos + 1) % DOA_NOISE_WIN_BLOCKS;
        if (e_hist_n < DOA_NOISE_WIN_BLOCKS)
            e_hist_n++;
        noise_db = e_hist[0];
        for (i = 1; i < e_hist_n; i++)
            if (e_hist[i] < noise_db)
                noise_db = e_hist[i];
    }

    if (!psd_init) {
        for (k = 0; k < DOA_NBINS_MAX; k++)
            psd_noise[k] = psd[k];
        for (k = k_lo; k <= k_hi; k++)
            psd_bg_db[k] = 10.0f * log10f(psd[k] + 1e-30f);
        psd_init = 1;
    } else if (!noise_freeze) {
        for (k = 0; k < DOA_NBINS_MAX; k++) {
            if (psd[k] < psd_noise[k])
                psd_noise[k] += 0.3f * (psd[k] - psd_noise[k]);
            else
                psd_noise[k] *= 1.0023f;            /* 约 0.5dB/s 缓升 */
        }
        /* dB 域平均：短时的强声音（刚响起的警笛）只让它升几 dB，持续的嗡嗡声、风扇声几秒后就成为背景 */
        for (k = k_lo; k <= k_hi; k++)
            psd_bg_db[k] += DOA_BG_ALPHA * (10.0f * log10f(psd[k] + 1e-30f) - psd_bg_db[k]);
    }
}

/* 谱特征：平坦度、质心、300~2500Hz 内最强峰的频率与"峰值/中位数"之比 */
static void spectral_features(doa_result_t *res)
{
    int k, i, j;
    int kf0 = hz2bin(300.0f), kf1 = hz2bin(8000.0f);
    int kc0 = hz2bin(100.0f), kc1 = hz2bin(10000.0f);
    int kp0 = hz2bin(300.0f), kp1 = hz2bin(2500.0f);
    double lsum = 0.0, sum = 0.0, wsum = 0.0, csum = 0.0;
    float tmp[64];
    int nt = 0, kmax = kp0;

    for (k = kf0; k <= kf1; k++) {
        lsum += log(psd[k] + 1e-30);
        sum  += psd[k];
    }
    res->flatness = (float)(exp(lsum / (kf1 - kf0 + 1)) / (sum / (kf1 - kf0 + 1) + 1e-30));

    for (k = kc0; k <= kc1; k++) {
        wsum += psd[k];
        csum += psd[k] * (double)k;
    }
    res->centroid_hz = (float)(csum / (wsum + 1e-30)) * DOA_FS_HZ / DOA_NFFT;

    for (k = kp0; k <= kp1; k++) {
        if (psd[k] > psd[kmax])
            kmax = k;
        if (nt < 64)
            tmp[nt++] = psd[k];
    }
    /* 中位数（选择排序前一半即可，nt <= 64） */
    for (i = 0; i <= nt / 2; i++)
        for (j = i + 1; j < nt; j++)
            if (tmp[j] < tmp[i]) {
                float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
    band_median = tmp[nt / 2];
    res->tonality_db = 10.0f * log10f(psd[kmax] / (tmp[nt / 2] + 1e-30f) + 1e-30f);
    res->peak_snr_db = psd_init ? 10.0f * log10f(psd[kmax] + 1e-30f) - psd_bg_db[kmax] : 0.0f;

    /* 对数功率抛物线插值求峰值频率 */
    {
        float y0 = logf(psd[kmax - 1] + 1e-30f);
        float y1 = logf(psd[kmax] + 1e-30f);
        float y2 = logf(psd[kmax + 1] + 1e-30f);
        float den = y0 - 2.0f * y1 + y2;
        float d = (den < 0.0f) ? 0.5f * (y0 - y2) / den : 0.0f;
        res->peak_hz = ((float)kmax + d) * DOA_FS_HZ / DOA_NFFT;
    }
}

/*
 * 持续噪声互功率谱的估计：某频点不高于两侧邻近频点（±3~±DOA_NX_NEIGH）平均值 DOA_NX_GATE_DB 时，
 * 认为该频点只有噪声，用本块互功率谱更新其指数平均。警笛等音调成分所在频点突出于两侧，不参与更新。
 * 风扇、车流、人声嘈杂等持续噪声的互功率谱稳定（方向固定或散射），减去后不再把聚焦方向拉过去。
 */
static void noise_xspec_update(void)
{
    const float gate = powf(10.0f, DOA_NX_GATE_DB / 10.0f);
    int k, p, a, b;

    psd_csum[0] = 0.0f;
    for (k = 0; k < DOA_NBINS_MAX; k++)
        psd_csum[k + 1] = psd_csum[k] + psd[k];
    for (k = k_lo; k <= k_hi; k++) {
        float side, alpha;
        int n;
        a = k - DOA_NX_NEIGH < 0 ? 0 : k - DOA_NX_NEIGH;
        b = k + DOA_NX_NEIGH >= DOA_NBINS_MAX ? DOA_NBINS_MAX - 1 : k + DOA_NX_NEIGH;
        side = psd_csum[b + 1] - psd_csum[a] - (psd_csum[k + 3] - psd_csum[k - 2]);
        n = (b - a + 1) - 5;
        if (n <= 0 || psd[k] > gate * side / (float)n)
            continue;
        if (nx_n[k] < 1000)
            nx_n[k]++;
        alpha = 1.0f / (float)nx_n[k];
        if (alpha < DOA_NX_ALPHA)
            alpha = DOA_NX_ALPHA;
        nx_psd[k] += alpha * (psd[k] - nx_psd[k]);
        for (p = 0; DOA_NX_SUBTRACT && p < num_pairs; p++) {
            const float *ar = sig_re[pair_i[p]], *ai = sig_im[pair_i[p]];
            const float *br = sig_re[pair_j[p]], *bi = sig_im[pair_j[p]];
            float cr = ar[k] * br[k] + ai[k] * bi[k];
            float ci = ai[k] * br[k] - ar[k] * bi[k];
            nx_re[p][k] += alpha * (cr - nx_re[p][k]);
            nx_im[p][k] += alpha * (ci - nx_im[p][k]);
        }
    }
}

/* 在 xcorr[][] 上做 0~360° SRP 扫描，抛物线插值细化；返回方位、峰值比与最大 SRP 值 */
static void srp_scan(float *az_out, float *pr_out, float *best_out)
{
    float best = -1e30f, sum = 0.0f, worst = 1e30f;
    int ibest = 0, a, p;

    for (a = 0; a < DOA_NUM_AZ; a++) {
        float s = 0.0f;
        for (p = 0; p < num_pairs; p++) {
            float tau = pair_dx[p] * az_cos[a] + pair_dy[p] * az_sin[a];
            float pos = (tau + max_lag) * DOA_LAG_UPSAMPLE;
            int   i0  = (int)pos;
            float fr  = pos - (float)i0;
            if (i0 < 0) { i0 = 0; fr = 0.0f; }
            if (i0 >= num_lags - 1) { i0 = num_lags - 2; fr = 1.0f; }
            s += xcorr[p][i0] * (1.0f - fr) + xcorr[p][i0 + 1] * fr;
        }
        srp[a] = s;
        sum += s;
        if (s > best) { best = s; ibest = a; }
        if (s < worst) worst = s;
    }
    {
        float y0 = srp[(ibest + DOA_NUM_AZ - 1) % DOA_NUM_AZ];
        float y1 = srp[ibest];
        float y2 = srp[(ibest + 1) % DOA_NUM_AZ];
        float den = y0 - 2.0f * y1 + y2;
        float delta = (den < 0.0f) ? 0.5f * (y0 - y2) / den : 0.0f;
        float az = (ibest + delta) * DOA_AZ_STEP_DEG;
        if (az < 0.0f)    az += 360.0f;
        if (az >= 360.0f) az -= 360.0f;
        *az_out = az;
    }
    *pr_out = (best - worst) / (sum / DOA_NUM_AZ - worst + 1e-20f);
    *best_out = best;
}

int doa_process(const uint32_t *pkt, int nframes, doa_result_t *res)
{
    int m, f, p, k, l;
    int n, nused;
    float energy = 0.0f;
    const float mask_lin = powf(10.0f, DOA_MASK_DB / 10.0f);

    res->valid = 0;
    res->azimuth_deg = 0.0f;
    res->peak_ratio = 0.0f;
    res->coherence = 0.0f;

    n = unpack(pkt, nframes, res);
    if (n < 0)
        return -1;

    /* 1. 去直流、短时峰值能量、加窗、FFT；平均功率谱与频带能量 */
    memset(psd, 0, sizeof(psd));
    memset(frame_e, 0, sizeof(frame_e));
    for (m = 0; m < num_mics; m++) {
        float mean = 0.0f;
        for (f = 0; f < n; f++)
            mean += sig_re[m][f];
        mean /= (float)n;
        /* 不加窗的短时能量：拍手只有几毫秒，加窗后的块能量会随它在块内的位置大幅变化 */
        for (f = 1; f < n; f++) {
            float d = sig_re[m][f] - 0.9f * sig_re[m][f - 1];   /* 预加重（约 800Hz 以下衰减），去掉低频隆隆声 */
            frame_e[f / DOA_PEAK_SUB] += d * d;
        }
        for (f = 0; f < DOA_NFFT; f++) {
            sig_re[m][f] = (f < n) ? (sig_re[m][f] - mean) * window[f] : 0.0f;
            sig_im[m][f] = 0.0f;
        }
        fft(sig_re[m], sig_im[m]);
        for (k = 1; k < DOA_NBINS_MAX; k++)
            psd[k] += sig_re[m][k] * sig_re[m][k] + sig_im[m][k] * sig_im[m][k];
    }
    for (k = 1; k < DOA_NBINS_MAX; k++) {
        psd[k] /= (float)num_mics;
        if (k >= k_lo && k <= k_hi)
            energy += psd[k];
    }
    res->energy_db = 10.0f * log10f(energy + 1e-20f);
    {
        float emax = 0.0f;
        for (f = 0; f + DOA_PEAK_NSUB <= DOA_NFFT / DOA_PEAK_SUB; f++) {
            float e = frame_e[f] + frame_e[f + 1] + frame_e[f + 2] + frame_e[f + 3];
            if (e > emax)
                emax = e;
        }
        float esum = 0.0f;
        for (f = 0; f < DOA_NFFT / DOA_PEAK_SUB; f++)
            esum += frame_e[f];
        res->peak_level_db = 10.0f * log10f(emax / (DOA_PEAK_SUB * DOA_PEAK_NSUB * num_mics) + 1e-20f);
        /* 峰均比：最强 128 点窗能量相对整块平均（同样折算到 128 点）的 dB 数。
         * 拍手能量集中在几毫秒内，约 10~15dB；语音、音乐、持续声音一般只有几 dB */
        res->crest_db = 10.0f * log10f(emax / (esum * DOA_PEAK_NSUB / (DOA_NFFT / DOA_PEAK_SUB)) + 1e-20f);
        for (f = 0; f < DOA_NSUB; f++)
            res->sub_e[f] = frame_e[f] / (float)num_mics;
    }
    spectral_features(res);

    /* 2. 频率掩码（用更新前的噪声估计，避免当前块自身拉高噪声） */
    nused = 0;
    for (k = k_lo; k <= k_hi; k++) {
        bin_mask[k] = psd_init && psd[k] > psd_noise[k] * mask_lin;
        nused += bin_mask[k];
    }
    if (nused < DOA_MASK_MIN_BINS) {          /* 有效频点太少时退回全频带 */
        for (k = k_lo; k <= k_hi; k++)
            bin_mask[k] = 1;
        nused = k_hi - k_lo + 1;
    }
    res->nbins = nused;
    update_noise(res->energy_db);
    res->noise_db = noise_db;

    /* 3. PHAT 加权互功率谱（掩码外频点置零） */
    for (p = 0; p < num_pairs; p++) {
        const float *ar = sig_re[pair_i[p]], *ai = sig_im[pair_i[p]];
        const float *br = sig_re[pair_j[p]], *bi = sig_im[pair_j[p]];
        for (k = k_lo; k <= k_hi; k++) {
            if (bin_mask[k]) {
                float cr = ar[k] * br[k] + ai[k] * bi[k];      /* X_i * conj(X_j) */
                float ci = ai[k] * br[k] - ar[k] * bi[k];
                float mag = sqrtf(cr * cr + ci * ci) + 1e-20f;
                g_re[p][k] = cr / mag;
                g_im[p][k] = ci / mag;
            } else {
                g_re[p][k] = 0.0f;
                g_im[p][k] = 0.0f;
            }
        }
    }

    /* 4. 细时延网格上的互相关 r(tau) = sum_k Re{G(k) e^{j 2 pi k tau / N}} */
    for (l = 0; l < num_lags; l++) {
        float tau = (float)(l - max_lag * DOA_LAG_UPSAMPLE) / DOA_LAG_UPSAMPLE;
        float ph0 = 2.0f * DOA_PI * k_lo * tau / DOA_NFFT;
        float phs = 2.0f * DOA_PI * tau / DOA_NFFT;
        float cr = cosf(ph0), ci = sinf(ph0);
        float sr = cosf(phs), si = sinf(phs);
        for (k = k_lo; k <= k_hi; k++) {
            float t;
            w_re[k] = cr;
            w_im[k] = ci;
            t  = cr * sr - ci * si;
            ci = cr * si + ci * sr;
            cr = t;
        }
        for (p = 0; p < num_pairs; p++) {
            float acc = 0.0f;
            for (k = k_lo; k <= k_hi; k++)
                acc += g_re[p][k] * w_re[k] - g_im[p][k] * w_im[k];
            xcorr[p][l] = acc;
        }
    }

    /* 5. 方位角扫描 */
    {
        float best;
        srp_scan(&res->azimuth_deg, &res->peak_ratio, &best);
        /* 相干度：最佳方向上各麦克风对的平均归一化相关值（0~1），直达声越强越高 */
        res->coherence = best / ((float)num_pairs * (float)nused);
    }

    res->valid = (res->energy_db - res->noise_db > DOA_DETECT_DB) &&
                 (res->peak_ratio > DOA_MIN_PEAK_RATIO);
    res->target_level_db = -200.0f;
    noise_xspec_update();
    return 0;
}

/*
 * 列出 [lo_hz, hi_hz]（限于 300~2500Hz）内的局部谱峰：频率（抛物线插值）与突出度
 * （相对 300~2500Hz 中位数，与 tonality_db 同一尺度），只保留突出度 >= min_prom_db 的。
 * 在 doa_process 之后调用。返回峰数。
 */
int doa_find_peaks(float lo_hz, float hi_hz, float min_prom_db, doa_peak_t *pk, int max_peaks)
{
    const float hz_per_bin = DOA_FS_HZ / DOA_NFFT;
    int kp0 = hz2bin(300.0f), kp1 = hz2bin(2500.0f);
    int k0 = hz2bin(lo_hz), k1 = hz2bin(hi_hz), k, n = 0;

    if (!psd_init)
        return 0;
    if (k0 < kp0 + 1) k0 = kp0 + 1;
    if (k1 > kp1 - 1) k1 = kp1 - 1;
    for (k = k0; k <= k1 && n < max_peaks; k++) {
        float prom;
        if (psd[k] < psd[k - 1] || psd[k] < psd[k + 1])
            continue;
        prom = 10.0f * log10f(psd[k] / (band_median + 1e-30f) + 1e-30f);
        if (prom < min_prom_db)
            continue;
        {
            float y0 = logf(psd[k - 1] + 1e-30f);
            float y1 = logf(psd[k] + 1e-30f);
            float y2 = logf(psd[k + 1] + 1e-30f);
            float den = y0 - 2.0f * y1 + y2;
            float d = (den < 0.0f) ? 0.5f * (y0 - y2) / den : 0.0f;
            pk[n].hz = ((float)k + d) * hz_per_bin;
        }
        pk[n].prominence_db = prom;
        n++;
    }
    return n;
}

/*
 * 目标频率聚焦定位（在 doa_process 之后调用，复用本块的 FFT 与功率谱）。
 *
 * 只用目标声音自己的频点定位，其他声源（说话、敲击、音乐）即使更响，
 * 只要不落在这些频点上，就不会把方向拉走：
 *   1. fp_hz 为选定的目标谱峰（由调用者按频率连续性从 doa_find_peaks 的结果中选出）
 *   2. 若该峰的 1/3 或 1/2 处也有突出峰，取为基频（实测警笛常是三次谐波最强）
 *   3. 取基频各整数倍（<= DOA_FREQ_HI_HZ）附近 ±DOA_FOCUS_HALF_BINS 个、且高于该频点背景的频点
 *   4. 只用这些频点做 GCC-PHAT + SRP
 */
int doa_focus_at(float fp_hz, doa_focus_t *out)
{
    const float mask_lin = powf(10.0f, DOA_MASK_DB / 10.0f);
    const float nx_sel = powf(10.0f, DOA_NX_SEL_DB / 10.0f);
    const float hz_per_bin = DOA_FS_HZ / DOA_NFFT;
    int kfp = (int)(fp_hz / hz_per_bin + 0.5f);
    int nb = 0, n, p, l, i, q;
    float fp = fp_hz, f0;

    memset(out, 0, sizeof(*out));
    out->azimuth_deg = -1.0f;
    if (!psd_init || kfp < 2 || kfp >= DOA_NBINS_MAX - 1)
        return 0;
    out->track_hz = fp;
    out->prominence_db = 10.0f * log10f(psd[kfp] / (band_median + 1e-30f) + 1e-30f);
    out->level_db = 10.0f * log10f(psd[kfp] + 1e-30f);
    out->snr_db = out->level_db - psd_bg_db[kfp];

    /* 2. 推断基频：1/3、1/2 处有突出峰则以它为基频 */
    f0 = fp;
    for (q = 3; q >= 2; q--) {
        float fc = fp / (float)q;
        int kc = (int)(fc / hz_per_bin + 0.5f), kk, found = 0;
        if (fc < 250.0f)
            continue;
        for (kk = kc - 1; kk <= kc + 1; kk++)
            if (kk > 1 && psd[kk] >= psd[kk - 1] && psd[kk] >= psd[kk + 1] &&
                10.0f * log10f(psd[kk] / (band_median + 1e-30f) + 1e-30f) >= DOA_FOCUS_SUB_PROM_DB)
                found = 1;
        if (found) {
            f0 = fc;
            break;
        }
    }
    out->f0_hz = f0;

    /*
     * 3. 基频各整数倍附近、高于背景的频点。该次谐波本身必须是突出的谱峰，
     *    否则这些频点上只有别的声源（如音乐的谐波），会把方向拉过去
     */
    for (n = 1; n * f0 <= DOA_FOCUS_MAX_HZ && nb < DOA_FOCUS_MAX_BINS; n++) {
        int kc = (int)(n * f0 / hz_per_bin + 0.5f), kn = kc, kb, kk;
        if (kc < 2 || kc >= DOA_NBINS_MAX - 2)
            continue;
        for (kk = kc - 1; kk <= kc + 1; kk++)
            if (psd[kk] > psd[kn])
                kn = kk;
        if (psd[kn] < psd[kn - 1] || psd[kn] < psd[kn + 1] ||
            10.0f * log10f(psd[kn] / (band_median + 1e-30f) + 1e-30f) < DOA_FOCUS_SUB_PROM_DB)
            continue;
        for (kb = kn - DOA_FOCUS_HALF_BINS; kb <= kn + DOA_FOCUS_HALF_BINS && nb < DOA_FOCUS_MAX_BINS; kb++) {
            if (kb < k_lo || kb > k_hi)
                continue;
            if (nb > 0 && focus_bins[nb - 1] >= kb)
                continue;                       /* 相邻谐波窗口重叠时不重复 */
            /* 还要明显高于持续噪声的平均功率：以噪声为主的频点（单块互功率谱起伏大）不用 */
            if (psd[kb] > psd_noise[kb] * mask_lin &&
                (!DOA_NX_SELECT || nx_n[kb] < DOA_NX_MIN_N || psd[kb] > nx_psd[kb] * nx_sel))
                focus_bins[nb++] = kb;
        }
    }
    out->nbins = nb;
    if (nb < DOA_FOCUS_MIN_BINS)
        return 0;
    {
        float pw = 0.0f;
        for (i = 0; i < nb; i++)
            pw += psd[focus_bins[i]];
        out->power_db = 10.0f * log10f(pw + 1e-30f);
    }

    /* 4. 只用这些频点的 GCC-PHAT 与 SRP */
    for (p = 0; p < num_pairs; p++) {
        const float *ar = sig_re[pair_i[p]], *ai = sig_im[pair_i[p]];
        const float *br = sig_re[pair_j[p]], *bi = sig_im[pair_j[p]];
        for (i = 0; i < nb; i++) {
            int kb = focus_bins[i];
            float cr = ar[kb] * br[kb] + ai[kb] * bi[kb];
            float ci = ai[kb] * br[kb] - ar[kb] * bi[kb];
            float mag = sqrtf(cr * cr + ci * ci) + 1e-20f;
            /* 减去持续噪声的互功率谱，再按本频点总功率归一化：
             * 无噪声时等同 PHAT；以噪声为主的频点剩下的很小，自动降权 */
            if (DOA_NX_SUBTRACT && nx_n[kb] >= DOA_NX_MIN_N) {
                cr -= nx_re[p][kb];
                ci -= nx_im[p][kb];
            }
            focus_gre[p][i] = cr / mag;
            focus_gim[p][i] = ci / mag;
        }
    }
    /* 各频点的相位随 lag 等步长变化：每个频点只算一次 cos/sin，之后逐 lag 旋转（板上省时） */
    for (p = 0; p < num_pairs; p++)
        for (l = 0; l < num_lags; l++)
            xcorr[p][l] = 0.0f;
    for (i = 0; i < nb; i++) {
        float ph0 = -2.0f * DOA_PI * focus_bins[i] * (float)max_lag / DOA_NFFT;
        float phs = 2.0f * DOA_PI * focus_bins[i] / ((float)DOA_NFFT * DOA_LAG_UPSAMPLE);
        float cr = cosf(ph0), ci = sinf(ph0);
        float sr = cosf(phs), si = sinf(phs);
        for (l = 0; l < num_lags; l++) {
            float t;
            for (p = 0; p < num_pairs; p++)
                xcorr[p][l] += focus_gre[p][i] * cr - focus_gim[p][i] * ci;
            t = cr * sr - ci * si;
            ci = cr * si + ci * sr;
            cr = t;
        }
    }
    {
        float best;
        srp_scan(&out->azimuth_deg, &out->peak_ratio, &best);
        out->coherence = best / ((float)num_pairs * (float)nb);
    }
    out->ok = 1;
    return 1;
}
