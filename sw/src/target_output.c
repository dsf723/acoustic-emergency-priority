#include "target_output.h"
#include "doa_config.h"
#include "dir_filter.h"
#include <math.h>

static int was_active, confirmed, support_n;
static float pending_az, confirmed_az;
static uint32_t last_support_blk, last_input_blk;
static int have_input;
static int disp_have;
static float avg_x, avg_y, disp_az;

static float difference(float a, float b)
{
    return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f);
}

void target_output_reset(void)
{
    was_active = confirmed = support_n = have_input = 0;
    last_support_blk = last_input_blk = 0;
    pending_az = confirmed_az = -1.0f;
    disp_have = 0;
    dir_filter_reset();
}

/* 显示用：长时圆周平均 + 死区 + 取整（参数见 doa_config.h TARGET_DISPLAY_*） */
static void display_update(float az, int restart)
{
    float a = az * 3.14159265f / 180.0f, est, q;

    if (restart || !disp_have) {
        avg_x = cosf(a);
        avg_y = sinf(a);
    } else {
        avg_x += TARGET_DISPLAY_ALPHA * (cosf(a) - avg_x);
        avg_y += TARGET_DISPLAY_ALPHA * (sinf(a) - avg_y);
    }
    est = atan2f(avg_y, avg_x) * 180.0f / 3.14159265f;
    if (est < 0.0f) est += 360.0f;
    if (restart || !disp_have || difference(est, disp_az) > TARGET_DISPLAY_DEADBAND_DEG) {
        q = floorf(est / TARGET_DISPLAY_STEP_DEG + 0.5f) * TARGET_DISPLAY_STEP_DEG;
        disp_az = q >= 360.0f ? q - 360.0f : q;
        disp_have = 1;
    }
}

void target_output_update(const doa_result_t *r, const snd_state_t *s,
                          uint32_t blk, target_output_t *out)
{
    int active = s->siren_active && s->cls == SND_SIREN;
    out->active = active;
    out->direction_valid = 0;
    out->azimuth_deg = -1.0f;
    out->band = BAND_NONE;
    if (!active) {
        target_output_reset();
        return;
    }
    if (!was_active) target_output_reset();
    was_active = 1;
    dir_filter_set_guard(1);
    if (confirmed && blk - last_support_blk > TARGET_DIR_FRESH_BLOCKS) {
        confirmed = support_n = 0;
    }
    if (s->dir_ok && isfinite(r->azimuth_deg) && r->azimuth_deg >= 0.0f &&
        r->azimuth_deg < 360.0f && isfinite(r->peak_ratio) &&
        r->peak_ratio >= SC_SIREN_MIN_PEAK_RATIO &&
        (!have_input || blk != last_input_blk)) {
        float az;
        last_input_blk = blk;
        have_input = 1;
        dir_filter_add(r->azimuth_deg, r->peak_ratio * r->peak_ratio, blk);
        az = dir_filter_get(blk);
        /* A rejected raw reflection must not refresh a held direction's age. */
        if (az >= 0.0f && difference(r->azimuth_deg, az) <= TARGET_DIR_CLUSTER_DEG) {
            if (!support_n || difference(az, pending_az) > TARGET_DIR_CLUSTER_DEG ||
                blk - last_support_blk > TARGET_DIR_FRESH_BLOCKS) {
                pending_az = az;
                support_n = 1;
            } else {
                if (support_n < TARGET_DIR_CONFIRM_BLOCKS) support_n++;
                pending_az = az;
            }
            last_support_blk = blk;
            if (support_n >= TARGET_DIR_CONFIRM_BLOCKS) {
                /* 重新确认、或方向滤波接受了大跳变（声源换了位置）：显示从新方向重新开始 */
                int restart = !confirmed || difference(az, confirmed_az) > DIR_FILTER_JUMP_DEG;
                confirmed_az = az;
                confirmed = 1;
                display_update(az, restart);
            }
        }
    }
    if (confirmed && blk - last_support_blk <= TARGET_DIR_FRESH_BLOCKS) {
        out->direction_valid = 1;
        out->azimuth_deg = disp_az;
        out->band = s->band;
    }
}
