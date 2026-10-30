#include "siren_focus.h"
#include "doa_config.h"
#include <math.h>

/*
 * 按方向分开的音调分量跟踪（"方向槽"）：
 *
 * 1. 每块列出 400~2500Hz 的突出谱峰，按得分排序：
 *      得分 = 突出度 + 连续性加分（与某个槽的预测频率相差 <= SF_MAX_STEP_HZ，或回到该槽
 *      SF_MODE_MIN_AGE 块以前用过的频率 ±SF_MODE_TOL_HZ 内——两音交替），目标槽加分更多，
 *      这样警笛分量即使不是最响的，也最先被聚焦定位。
 *    依次做聚焦定位（最多 SF_MAX_FOCUS 次），已定位分量的谐波跳过。
 * 2. 每个方向明确（峰值比 > SC_SIREN_MIN_PEAK_RATIO）的分量按方位归入 ±SF_DIR_CLUSTER_DEG 内的槽，
 *    没有就开新槽；每槽每块最多一个观测（得分最高的那个）。
 * 3. 每槽自己记最近 SF_WIN 块（约 1 秒）的：有无音调、频率、谱峰电平，判据与 sound_class 相同：
 *      音调块占比 >= SF_STRICT_FRAC、频率 10%~90% 分位差 >= SC_SIREN_MIN_SPAN_HZ、
 *      电平 10%~90% 分位差 <= SC_SIREN_MAX_ERANGE_DB（拨弦/钢琴音乐的每个音都在衰减）；
 *    另加频率变化规律：警笛要么连续升降，要么在用过的两个音之间来回跳；音乐不断跳到新的音高。
 *    "无法解释的跳变"（既不连续，也不是回到该槽用过的频率或其整数倍）超过 SF_MAX_JUMPS 次的方向不是警笛。
 *    没有目标时，选通过判据、占比最高的槽作目标；之后目标保持不变（滞回），
 *    直到其占比降到 SC_SIREN_OFF_FRAC 以下，或频率连续 SC_SIREN_PATTERN_HOLD_BLOCKS 块没有起伏。
 *    宽带识别已确认警笛、却没有严格符合的方向时（混响房间里窄带声音方向较散），用备用目标（见 update_target）。
 * 4. 识别（是不是警笛）仍由 sound_class 按宽带特征判断；方向、峰值比、相干度（远近）只取目标槽的观测，
 *    本块目标槽没有观测时不给方向，绝不退回宽带结果。
 *    所以无论干扰比警笛先出现还是更响，显示方向都只来自"听起来像警笛"的那个方向。
 */
#if DOA_FOCUS_ENABLE
typedef struct {
    int   used;
    float az;                       /* 槽方向（圆周平滑，跟随缓慢移动的声源） */
    float track_hz, vel_hz;         /* 最近一次观测频率与每块变化 */
    int   n, pos;
    unsigned char flag[SF_WIN];
    unsigned char jump[SF_WIN];     /* 该块频率既不连续、也不是回到用过的频率（或其整数倍） */
    float hz[SF_WIN], lvl[SF_WIN];
    int   bad_blocks;               /* 作为目标时频率没有起伏的连续块数 */
    int   obs;                      /* 本块是否已有观测 */
    int   member;                   /* 本块观测属于本槽的频率轨迹（不是离群点） */
    float cand_hz;                  /* 离群点：可能的新频率 */
    int   cand_n;
    float obs_score;
    doa_focus_t foc;
} slot_t;

static slot_t slot[SF_SLOTS];
#endif
static int target = -1;
static int target_strict;          /* 1：目标槽本身完全符合警笛判据 */

void siren_focus_reset(void)
{
#if DOA_FOCUS_ENABLE
    int s;
    for (s = 0; s < SF_SLOTS; s++)
        slot[s].used = 0;
#endif
    target = -1;
    target_strict = 0;
}

#if DOA_FOCUS_ENABLE
typedef struct {
    float hz;
    float score;
} cand_t;

static float ang_diff(float a, float b)
{
    return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f);
}

static void sort_f(float *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        float v = a[i];
        for (j = i - 1; j >= 0 && a[j] > v; j--)
            a[j + 1] = a[j];
        a[j + 1] = v;
    }
}

/* hz 是否接近槽 s 在 SF_MODE_MIN_AGE 块以前用过的某个频率 */
static int near_mode(const slot_t *s, float hz)
{
    int i;
    for (i = SF_MODE_MIN_AGE; i < s->n; i++) {
        int k = (s->pos - 1 - i + 2 * SF_WIN) % SF_WIN;
        if (s->flag[k] && fabsf(s->hz[k] - hz) <= SF_MODE_TOL_HZ)
            return 1;
    }
    return 0;
}

/* 槽 s 对频率 hz 的连续性加分（不连续返回 0） */
static float cont_bonus(int s, float hz)
{
    const slot_t *sl = &slot[s];
    float full = (s == target) ? SF_TARGET_BONUS_DB : SF_SLOT_BONUS_DB;
    float d = fabsf(hz - (sl->track_hz + sl->vel_hz)), b = 0.0f;

    if (d <= SF_MAX_STEP_HZ)
        b = full - SF_PENALTY_DB_PER_HZ * d;
    else if (near_mode(sl, hz))
        b = full - SF_MODE_JUMP_COST_DB;
    return b > 0.0f ? b : 0.0f;
}

/* f 与 g 是否约成整数倍关系（警笛跟踪的谱峰在基频和谐波之间换） */
static int harmonic_ratio(float f, float g)
{
    float r, m;
    if (f <= 0.0f || g <= 0.0f)
        return 0;
    r = f > g ? f / g : g / f;
    m = floorf(r + 0.5f);
    return m >= 2.0f && fabsf(r - m) <= 0.04f * m;
}

/* 槽统计：音调块占比、频率与电平的 10%~90% 分位差、无法解释的跳变次数 */
static void slot_stats(const slot_t *s, float *frac, float *span, float *erange, int *jumps)
{
    float hz[SF_WIN], e[SF_WIN];
    int i, n = 0;

    *jumps = 0;
    for (i = 0; i < s->n; i++)
        if (s->flag[i]) {
            *jumps += s->jump[i];
            hz[n] = s->hz[i];
            e[n] = s->lvl[i];
            n++;
        }
    *frac = s->n ? (float)n / (float)s->n : 0.0f;
    *span = 0.0f;
    *erange = 100.0f;
    if (n < 4)
        return;
    sort_f(hz, n);
    sort_f(e, n);
    *span = hz[(n * 9) / 10] - hz[n / 10];
    *erange = e[(n * 9) / 10] - e[n / 10];
}

/* 方位 az 所属的槽；没有就开一个新槽（替换音调最少的非目标槽）。返回 -1 表示无槽可用 */
static int slot_for(float az)
{
    int s, best = -1, worst = -1, worst_cnt = SF_WIN + 1;
    float bd = SF_DIR_CLUSTER_DEG;

    for (s = 0; s < SF_SLOTS; s++) {
        if (slot[s].used) {
            float d = ang_diff(az, slot[s].az);
            if (d <= bd) {
                bd = d;
                best = s;
            }
        }
    }
    if (best >= 0)
        return best;
    for (s = 0; s < SF_SLOTS; s++) {
        int i, cnt = 0;
        if (!slot[s].used) {
            worst = s;
            break;
        }
        if (s == target)
            continue;
        for (i = 0; i < slot[s].n; i++)
            cnt += slot[s].flag[i];
        if (cnt < worst_cnt) {
            worst_cnt = cnt;
            worst = s;
        }
    }
    if (worst >= 0) {
        slot[worst].used = 1;
        slot[worst].az = az;
        slot[worst].track_hz = -1000.0f;
        slot[worst].vel_hz = 0.0f;
        slot[worst].n = slot[worst].pos = 0;
        slot[worst].bad_blocks = 0;
        slot[worst].obs = 0;
        slot[worst].cand_n = 0;
    }
    return worst;
}

/* 槽方向向 az 挪一点（圆周加权） */
static void az_follow(slot_t *s, float az)
{
    float a = s->az * 3.14159265f / 180.0f, b = az * 3.14159265f / 180.0f;
    float x = (1.0f - SF_DIR_FOLLOW) * cosf(a) + SF_DIR_FOLLOW * cosf(b);
    float y = (1.0f - SF_DIR_FOLLOW) * sinf(a) + SF_DIR_FOLLOW * sinf(b);
    s->az = atan2f(y, x) * 180.0f / 3.14159265f;
    if (s->az < 0.0f)
        s->az += 360.0f;
}

/* hz 是否为已定位分量（基频 f0）的谐波 */
static int is_harmonic(float hz, const float *f0, int n)
{
    const float tol = DOA_FS_HZ / DOA_NFFT;
    int i;
    for (i = 0; i < n; i++) {
        float m = floorf(hz / f0[i] + 0.5f);
        if (m >= 1.0f && fabsf(hz - m * f0[i]) <= tol)
            return 1;
    }
    return 0;
}

/* 本块：找分量、聚焦定位、归槽、更新各槽统计 */
static void observe(void)
{
    doa_peak_t pk[48];
    cand_t c[48];
    float f0s[SF_MAX_FOCUS];
    int n, i, j, s, nf = 0;
    int member;

    n = doa_find_peaks(SC_SIREN_F_LO_HZ, SC_SIREN_F_HI_HZ, DOA_FOCUS_MIN_PROM_DB, pk, 48);
    for (i = 0; i < n; i++) {
        float b = 0.0f;
        for (s = 0; s < SF_SLOTS; s++)
            if (slot[s].used) {
                float bs = cont_bonus(s, pk[i].hz);
                if (bs > b)
                    b = bs;
            }
        c[i].hz = pk[i].hz;
        c[i].score = pk[i].prominence_db + b;
    }
    for (i = 1; i < n; i++) {               /* 按得分从高到低 */
        cand_t v = c[i];
        for (j = i - 1; j >= 0 && c[j].score < v.score; j--)
            c[j + 1] = c[j];
        c[j + 1] = v;
    }

    for (s = 0; s < SF_SLOTS; s++)
        slot[s].obs = 0;
    for (i = 0; i < n && nf < SF_MAX_FOCUS; i++) {
        doa_focus_t foc;
        if (is_harmonic(c[i].hz, f0s, nf))
            continue;
        if (!doa_focus_at(c[i].hz, &foc)) {
            f0s[nf++] = c[i].hz;
            continue;
        }
        f0s[nf++] = foc.f0_hz;
        if (foc.peak_ratio <= SC_SIREN_MIN_PEAK_RATIO)
            continue;                       /* 方向不明确，不归槽 */
        s = slot_for(foc.azimuth_deg);
        if (s < 0 || slot[s].obs)
            continue;
        slot[s].obs = 1;
        slot[s].obs_score = c[i].score;
        slot[s].foc = foc;
    }

    for (s = 0; s < SF_SLOTS; s++) {
        slot_t *sl = &slot[s];
        int cnt = 0;
        if (!sl->used)
            continue;
        /*
         * 观测归属：与本槽频率轨迹连续（或回到用过的频率 / 整数倍）的是"成员"；否则先当离群点
         * （两个声源频点重叠时，别的声源的分量偶尔被定位到这个方向），不进统计、不改轨迹；
         * 连续 SF_REANCHOR_N 个离群点彼此连续，才认为频率真的跳了（记一次跳变），轨迹换过去。
         */
        member = 0;
        sl->jump[sl->pos] = 0;
        if (sl->obs) {
            float f = sl->foc.track_hz;
            if (sl->track_hz <= 0.0f ||
                fabsf(f - (sl->track_hz + sl->vel_hz)) <= SF_MAX_STEP_HZ ||
                near_mode(sl, f) || harmonic_ratio(f, sl->track_hz) ||
                harmonic_ratio(f, sl->track_hz + sl->vel_hz)) {
                member = 1;
                if (sl->track_hz > 0.0f && fabsf(f - sl->track_hz) <= SF_MAX_STEP_HZ)
                    sl->vel_hz = 0.7f * sl->vel_hz + 0.3f * (f - sl->track_hz);
                else
                    sl->vel_hz = 0.0f;      /* 跳变（两音交替、换谐波）：不当作连续变化 */
                sl->track_hz = f;
                sl->cand_n = 0;
            } else {
                if (sl->cand_n > 0 && fabsf(f - sl->cand_hz) <= SF_MAX_STEP_HZ)
                    sl->cand_n++;
                else
                    sl->cand_n = 1;
                sl->cand_hz = f;
                if (sl->cand_n >= SF_REANCHOR_N) {
                    member = 1;
                    sl->jump[sl->pos] = 1;
                    sl->track_hz = f;
                    sl->vel_hz = 0.0f;
                    sl->cand_n = 0;
                }
            }
        }
        sl->flag[sl->pos] = (unsigned char)member;
        sl->hz[sl->pos] = member ? sl->foc.track_hz : 0.0f;
        sl->lvl[sl->pos] = member ? sl->foc.level_db : 0.0f;
        sl->pos = (sl->pos + 1) % SF_WIN;
        if (sl->n < SF_WIN)
            sl->n++;
        sl->member = member;
        if (member) {
            az_follow(sl, sl->foc.azimuth_deg);
        } else {
            sl->track_hz += sl->vel_hz;
            sl->vel_hz *= 0.9f;
        }
        for (i = 0; i < sl->n; i++)
            cnt += sl->flag[i];
        if (sl->n == SF_WIN && cnt == 0) {
            sl->used = 0;                   /* 这个方向已经安静了 1 秒 */
            if (s == target)
                target = -1;
        }
    }
}

/* 槽 s 是否完全符合警笛判据 */
static int slot_siren_like(int s)
{
    float frac, span, erange;
    int jumps;
    slot_stats(&slot[s], &frac, &span, &erange, &jumps);
    return frac >= SF_STRICT_FRAC && span >= SC_SIREN_MIN_SPAN_HZ &&
           erange <= SC_SIREN_MAX_ERANGE_DB && jumps <= SF_MAX_JUMPS;
}

/*
 * 备用目标得分：音调块占比 × 响度平稳程度（电平范围 <= SC_SIREN_MAX_ERANGE_DB 记 1，到 +12dB 降为 0，
 * 拨弦/钢琴音乐每个音都在衰减，这一项很低）- 每次无法解释的跳变扣 0.1
 */
static float fallback_score(int s)
{
    float frac, span, erange, st;
    int jumps;
    slot_stats(&slot[s], &frac, &span, &erange, &jumps);
    if (frac < SF_FALLBACK_FRAC)
        return 0.0f;
    st = 1.0f - (erange - SC_SIREN_MAX_ERANGE_DB) / 12.0f;
    if (st > 1.0f) st = 1.0f;
    if (st < 0.0f) st = 0.0f;
    return frac * st - 0.1f * (float)jumps;
}

/*
 * 目标槽的保持与选择。
 * 严格目标：槽本身完全符合警笛判据。
 * 备用目标：警笛已由宽带识别确认、但没有严格符合的方向时（混响房间里窄带声音的方向较散，
 *   单个方向的音调块占比不够），取 fallback_score 最高的槽；已有备用目标时新方向得分须高出
 *   SF_FALLBACK_SWITCH 才换；一旦出现严格目标就换过去，警笛结束即放弃。
 */
static void update_target(int siren_active)
{
    float frac, span, erange, best = 0.0f;
    int s, jumps, pick = -1;

    if (target >= 0) {
        slot_stats(&slot[target], &frac, &span, &erange, &jumps);
        /* 警笛仍被确认时，严格目标只在占比极低时才放弃：干扰期间警笛分量暂时被盖住，不能换成干扰的方向 */
        float off = (target_strict && siren_active) ? SF_HOLD_FRAC : SC_SIREN_OFF_FRAC;
        if (frac < off || (!target_strict && !siren_active)) {
            target = -1;
        } else if (span < SC_SIREN_MIN_SPAN_HZ) {
            if (++slot[target].bad_blocks >= SC_SIREN_PATTERN_HOLD_BLOCKS)
                target = -1;                /* 变成了稳定的单音（口哨、喇叭），不再是警笛 */
        } else {
            slot[target].bad_blocks = 0;
        }
    }
    if (target >= 0 && target_strict)
        return;
    for (s = 0; s < SF_SLOTS; s++) {
        if (!slot[s].used || slot[s].n < SF_MIN_N || !slot_siren_like(s))
            continue;
        slot_stats(&slot[s], &frac, &span, &erange, &jumps);
        if (frac > best) {
            best = frac;
            pick = s;
        }
    }
    if (pick >= 0) {
        target = pick;
        target_strict = 1;
        slot[target].bad_blocks = 0;
        return;
    }
    if (!siren_active)
        return;
    /* 备用目标：按得分选，已有备用目标时只有明显更好的方向才换 */
    for (s = 0; s < SF_SLOTS; s++) {
        float sc;
        if (!slot[s].used || slot[s].n < SF_MIN_N)
            continue;
        sc = fallback_score(s);
        if (sc > best) {
            best = sc;
            pick = s;
        }
    }
    if (pick < 0 || best < SF_FALLBACK_MIN_SCORE || pick == target)
        return;
    if (target >= 0 && best < fallback_score(target) + SF_FALLBACK_SWITCH)
        return;
    target = pick;
    target_strict = 0;
    slot[target].bad_blocks = 0;
}
#endif

void siren_focus_apply(doa_result_t *r, int siren_active, siren_focus_info_t *info)
{
    info->raw_azimuth = r->azimuth_deg;
    info->raw_peak_ratio = r->peak_ratio;
    info->focused = 0;
    info->focus_azimuth = -1.0f;
    info->focus_peak_ratio = 0.0f;

#if DOA_FOCUS_ENABLE
    observe();
    update_target(siren_active);

    if (target >= 0 && slot[target].obs && slot[target].member) {
        const doa_focus_t *foc = &slot[target].foc;
        info->focused = 1;
        info->focus_azimuth = foc->azimuth_deg;
        info->focus_peak_ratio = foc->peak_ratio;
        /* 方向、远近（相干度）只来自目标方向的分量；识别仍用宽带特征，频率跟着目标分量 */
        r->peak_hz = foc->track_hz;
        if (foc->prominence_db > r->tonality_db)
            r->tonality_db = foc->prominence_db;
        r->coherence = foc->coherence;
        r->azimuth_deg = foc->azimuth_deg;
        r->peak_ratio = foc->peak_ratio;
        r->peak_snr_db = foc->snr_db;
        r->target_level_db = foc->power_db;
        r->valid = foc->snr_db > DOA_DETECT_DB || r->energy_db - r->noise_db > DOA_DETECT_DB;
    } else if (target >= 0 || siren_active) {
        /* 本块没有目标方向的分量：不给方向，绝不退回宽带结果（宽带方向会被干扰源拉走） */
        r->peak_ratio = 0.0f;
        r->valid = 0;
    }
    info->locked = target >= 0;
    info->track_hz = target >= 0 ? slot[target].track_hz : -1.0f;
#else
    (void)siren_active;
    info->locked = 0;
    info->track_hz = -1.0f;
#endif
}
