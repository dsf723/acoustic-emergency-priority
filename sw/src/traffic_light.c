#include "traffic_light.h"
#include <math.h>

static void enter(tl_state_t *tl, int state, uint32_t now, uint32_t dur)
{
    tl->state = state;
    tl->t_state = now;
    tl->dur = dur;
}

void tl_init(tl_state_t *tl, uint32_t now_ms)
{
    tl->phase = TL_PH_NS;
    enter(tl, TL_ST_GREEN, now_ms, TL_GREEN_MS);
    tl->preempt = 0;
    tl->approach = -1;
    tl->last_req = now_ms;
    tl->pend_approach = -1;
    tl->pend_since = now_ms;
}

int tl_phase_of(int approach)
{
    return (approach == TL_N || approach == TL_S) ? TL_PH_NS : TL_PH_EW;
}

int tl_approach_from_az(float az_deg)
{
    float rel = fmodf(az_deg - TL_NORTH_AZ_DEG + 720.0f, 360.0f);
    if (rel < 45.0f || rel >= 315.0f)
        return TL_N;
    if (rel < 135.0f)
        return TL_W;                    /* 逆时针 90° */
    if (rel < 225.0f)
        return TL_S;
    return TL_E;
}

const char *tl_approach_name(int approach)
{
    static const char *n[TL_NUM_APPROACH] = { "北", "南", "东", "西" };
    return (approach >= 0 && approach < TL_NUM_APPROACH) ? n[approach] : "";
}

/* 优先请求的接收、改向确认与结束 */
static void update_request(tl_state_t *tl, uint32_t now, int req)
{
    if (req >= 0) {
        tl->last_req = now;
        if (!tl->preempt) {
            tl->preempt = 1;
            tl->approach = req;
            tl->pend_approach = -1;
        } else if (tl_phase_of(req) == tl_phase_of(tl->approach)) {
            tl->approach = req;         /* 同一相位（如北改南）直接更新 */
            tl->pend_approach = -1;
        } else if (req != tl->pend_approach) {
            tl->pend_approach = req;
            tl->pend_since = now;
        } else if (now - tl->pend_since >= TL_SWITCH_CONFIRM_MS) {
            tl->approach = req;
            tl->pend_approach = -1;
        }
    } else if (tl->preempt && now - tl->last_req >= TL_PREEMPT_HOLD_MS) {
        tl->preempt = 0;
        tl->approach = -1;
        tl->pend_approach = -1;
    }
}

int tl_step(tl_state_t *tl, uint32_t now_ms, int req_approach)
{
    int old_state = tl->state, old_phase = tl->phase, old_pre = tl->preempt;
    int want;
    uint32_t el;

    update_request(tl, now_ms, req_approach);
    want = tl->preempt ? tl_phase_of(tl->approach) : -1;
    el = now_ms - tl->t_state;

    switch (tl->state) {
    case TL_ST_GREEN:
        if (want == tl->phase) {
            tl->dur = 0;                                /* 绿灯延长：保持 */
        } else {
            if (want >= 0) {                            /* 红灯早断：当前绿灯提前结束，结束前也要闪够 */
                uint32_t end = el + TL_GREEN_FLASH_MS;
                if (end < TL_MIN_GREEN_MS)
                    end = TL_MIN_GREEN_MS;
                if (tl->dur == 0 || tl->dur > end)
                    tl->dur = end;
            } else if (tl->dur == 0) {                  /* 优先刚结束：再放一会儿 */
                tl->dur = el + TL_RELEASE_GREEN_MS;
            }
            if (el >= tl->dur)
                enter(tl, TL_ST_YELLOW, now_ms, TL_YELLOW_MS);
        }
        break;
    case TL_ST_YELLOW:
        if (el >= TL_YELLOW_MS)
            enter(tl, TL_ST_ALLRED, now_ms, TL_ALLRED_MS);
        break;
    default:
        if (el >= TL_ALLRED_MS) {
            /* 优先时直接放来车相位（可能再次放刚结束的相位），否则轮换 */
            tl->phase = want >= 0 ? want : 1 - tl->phase;
            enter(tl, TL_ST_GREEN, now_ms, want == tl->phase ? 0 : TL_GREEN_MS);
        }
        break;
    }
    return tl->state != old_state || tl->phase != old_phase || tl->preempt != old_pre;
}

int tl_light(const tl_state_t *tl, int approach)
{
    if (tl_phase_of(approach) != tl->phase || tl->state == TL_ST_ALLRED)
        return TL_RED;
    return tl->state == TL_ST_GREEN ? TL_GREEN : TL_YELLOW;
}

/* 绿灯闪烁的灭相：定时绿灯的最后 TL_GREEN_FLASH_MS，亮灭各 0.5s，优先保持（不定时）时不闪 */
static int green_flash_off(const tl_state_t *tl, uint32_t now_ms)
{
    uint32_t el = now_ms - tl->t_state, left;
    if (tl->state != TL_ST_GREEN || tl->dur == 0 || el >= tl->dur)
        return 0;
    left = tl->dur - el;
    return left <= TL_GREEN_FLASH_MS && left % 1000u < 500u;
}

uint32_t tl_outputs(const tl_state_t *tl, uint32_t now_ms)
{
    uint32_t out = 0;
    int off = green_flash_off(tl, now_ms);
    int a;
    for (a = 0; a < TL_NUM_APPROACH; a++) {
        int l = tl_light(tl, a);
        if (l != TL_GREEN || !off)
            out |= 1u << (3 * a + l);               /* 红 +0、黄 +1、绿 +2 */
    }
    return out;
}

static int ceil_s(uint32_t ms)
{
    return (int)((ms + 999u) / 1000u);
}

int tl_countdown_s(const tl_state_t *tl, int ph, uint32_t now_ms)
{
    uint32_t el = now_ms - tl->t_state, left;
    int want = tl->preempt ? tl_phase_of(tl->approach) : -1;

    left = tl->dur > el ? tl->dur - el : 0;
    if (tl->state == TL_ST_GREEN && tl->dur == 0)
        return -1;                                  /* 优先保持中，不定时 */
    if (ph == tl->phase && tl->state != TL_ST_ALLRED)
        return ceil_s(left);                        /* 本相位绿/黄的剩余 */
    /* 本相位是红灯：到它变绿还要多久 */
    if (want >= 0 && want != ph)
        return -1;                                  /* 优先放行另一方向，红灯保持 */
    if (tl->state == TL_ST_ALLRED)
        return ph != tl->phase || want == ph ? ceil_s(left) :
               ceil_s(left + TL_GREEN_MS + TL_YELLOW_MS + TL_ALLRED_MS);
    if (tl->state == TL_ST_GREEN)
        return ceil_s(left + TL_YELLOW_MS + TL_ALLRED_MS);
    return ceil_s(left + TL_ALLRED_MS);             /* 另一相位黄灯中 */
}
