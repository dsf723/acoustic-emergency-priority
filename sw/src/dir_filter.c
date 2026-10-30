#include "dir_filter.h"
#include <math.h>

#define PI_F 3.14159265f

typedef struct {
    float    az;
    float    w;
    uint32_t blk;
} det_t;

static det_t hist[DIR_FILTER_SIZE];
static int   hist_n, hist_pos;
static int   guard_on;
static float held_az;
static uint32_t held_blk;

static float ang_diff(float a, float b)
{
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return fabsf(d);
}

void dir_filter_reset(void)
{
    hist_n = 0;
    hist_pos = 0;
    guard_on = 0;
    held_az = -1.0f;
    held_blk = 0;
}

void dir_filter_set_guard(int active)
{
    guard_on = active != 0;
}

void dir_filter_add(float az_deg, float weight, uint32_t blk)
{
    if (!isfinite(az_deg) || !isfinite(weight) || weight <= 0.0f)
        return;
    az_deg = fmodf(az_deg, 360.0f);
    if (az_deg < 0.0f) az_deg += 360.0f;
    if (weight > DIR_FILTER_WEIGHT_MAX) weight = DIR_FILTER_WEIGHT_MAX;
    hist[hist_pos].az = az_deg;
    hist[hist_pos].w = weight;
    hist[hist_pos].blk = blk;
    hist_pos = (hist_pos + 1) % DIR_FILTER_SIZE;
    if (hist_n < DIR_FILTER_SIZE)
        hist_n++;
}

float dir_filter_get(uint32_t blk)
{
    int i, j, best = -1;
    float best_w = 0.0f;
    float sx = 0.0f, sy = 0.0f;
    float out;

    /* 以每个检测为中心，统计 ±CLUSTER 内的权重和，取最大者 */
    for (i = 0; i < hist_n; i++) {
        float w = 0.0f;
        if (blk - hist[i].blk > DIR_FILTER_WINDOW)
            continue;
        for (j = 0; j < hist_n; j++) {
            if (blk - hist[j].blk > DIR_FILTER_WINDOW)
                continue;
            if (ang_diff(hist[i].az, hist[j].az) <= DIR_FILTER_CLUSTER_DEG)
                w += hist[j].w;
        }
        if (w > best_w) {
            best_w = w;
            best = i;
        }
    }
    if (best < 0) {
        held_az = -1.0f;
        return -1.0f;
    }

    /* 该簇的加权圆周平均 */
    for (j = 0; j < hist_n; j++) {
        if (blk - hist[j].blk > DIR_FILTER_WINDOW)
            continue;
        if (ang_diff(hist[best].az, hist[j].az) <= DIR_FILTER_CLUSTER_DEG) {
            float a = hist[j].az * PI_F / 180.0f;
            sx += hist[j].w * cosf(a);
            sy += hist[j].w * sinf(a);
        }
    }
    out = atan2f(sy, sx) * 180.0f / PI_F;
    if (out < 0.0f) out += 360.0f;
    if (guard_on && held_az >= 0.0f && blk - held_blk <= DIR_FILTER_WINDOW &&
        ang_diff(out, held_az) > DIR_FILTER_JUMP_DEG) {
        unsigned oldest = 0, newest = DIR_FILTER_CONFIRM_WIN + 1;
        int count = 0;
        float old_w = 0.0f;
        for (j = 0; j < hist_n; j++) {
            unsigned age = blk - hist[j].blk;
            int duplicate = 0;
            if (age > DIR_FILTER_WINDOW) continue;
            if (ang_diff(held_az, hist[j].az) <= DIR_FILTER_CLUSTER_DEG)
                old_w += hist[j].w;
            if (age > DIR_FILTER_CONFIRM_WIN ||
                ang_diff(hist[best].az, hist[j].az) > DIR_FILTER_CLUSTER_DEG)
                continue;
            /* Multiple additions in one DMA block count as one observation. */
            for (i = 0; i < j; i++)
                if (hist[i].blk == hist[j].blk &&
                    ang_diff(hist[best].az, hist[i].az) <= DIR_FILTER_CLUSTER_DEG)
                    duplicate = 1;
            if (duplicate) continue;
            count++;
            if (age > oldest) oldest = age;
            if (age < newest) newest = age;
        }
        if (count < DIR_FILTER_CONFIRM_N || newest > 3 ||
            oldest - newest < DIR_FILTER_CONFIRM_SPAN ||
            best_w < old_w * DIR_FILTER_SWITCH_RATIO)
            return held_az; /* Do not refresh its timestamp with rejected evidence. */
    }
    held_az = out;
    held_blk = blk;
    return out;
}
