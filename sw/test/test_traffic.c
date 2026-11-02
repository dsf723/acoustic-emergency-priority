/* 路口信号灯控制（traffic_light.c）的 PC 测试 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "traffic_light.h"

#define DT 20u                          /* 每步 20ms，接近板上每块 21ms */

static tl_state_t tl;
static uint32_t now;
static int last_light[TL_NUM_APPROACH];
static uint32_t light_since[TL_NUM_APPROACH];

/* 每步检查安全不变量 */
static void check_invariants(void)
{
    int a, ns = 0, ew = 0;
    for (a = 0; a < TL_NUM_APPROACH; a++) {
        int l = tl_light(&tl, a);
        uint32_t bits = (tl_outputs(&tl, now) >> (3 * a)) & 7u;
        if (l != TL_RED) {
            if (tl_phase_of(a) == TL_PH_NS) ns = 1; else ew = 1;
        }
        if (l != last_light[a]) {
            uint32_t held = now - light_since[a];
            if (last_light[a] == TL_GREEN) assert(l == TL_YELLOW);         /* 绿后必黄 */
            if (last_light[a] == TL_YELLOW) {
                assert(l == TL_RED);                                       /* 黄后必红 */
                assert(held + DT >= TL_YELLOW_MS);
            }
            if (l == TL_GREEN) assert(last_light[a] == TL_RED);
            last_light[a] = l;
            light_since[a] = now;
        }
        /* 只亮当前灯色那一颗；绿灯、黄灯闪烁的灭相可以全灭 */
        if (l == TL_GREEN)
            assert(bits == 4u || bits == 0u);
        else if (l == TL_YELLOW)
            assert(bits == 2u || bits == 0u);
        else
            assert(bits == 1u);
    }
    assert(!(ns && ew));                                                   /* 不会两个方向同时放行 */
    assert((tl_outputs(&tl, now) >> 12) == 0u);                            /* 没有优先指示灯 */
}

static void reset(void)
{
    int a;
    now = 0;
    tl_init(&tl, now);
    for (a = 0; a < TL_NUM_APPROACH; a++) {
        last_light[a] = tl_light(&tl, a);
        light_since[a] = 0;
    }
}

static void run(uint32_t ms, int req)
{
    uint32_t end = now + ms;
    while (now < end) {
        now += DT;
        tl_step(&tl, now, req);
        check_invariants();
    }
}

/* 运行 ms，统计北口某颗灯（mask）输出的翻转次数 */
static int count_toggles(uint32_t mask, uint32_t ms, int req)
{
    uint32_t end = now + ms, last = tl_outputs(&tl, now) & mask;
    int n = 0;
    while (now < end) {
        now += DT;
        tl_step(&tl, now, req);
        check_invariants();
        if ((tl_outputs(&tl, now) & mask) != last) {
            last = tl_outputs(&tl, now) & mask;
            n++;
        }
    }
    return n;
}

static int count_green_toggles(uint32_t ms, int req)
{
    return count_toggles(4u, ms, req);
}

int main(void)
{
    int i;
    uint32_t t0;

    /* 方位换算：0° 北、逆时针 90° 西、180° 南、270° 东 */
    assert(tl_approach_from_az(10) == TL_N && tl_approach_from_az(350) == TL_N);
    assert(tl_approach_from_az(90) == TL_W && tl_approach_from_az(180) == TL_S && tl_approach_from_az(270) == TL_E);

    /* 1. 正常循环：南北绿 25s（最后 3s 闪）→ 黄 3s → 全红 1s → 东西绿 */
    reset();
    assert(tl.phase == TL_PH_NS && tl.state == TL_ST_GREEN && !tl.preempt);
    assert(tl_countdown_s(&tl, TL_PH_NS, now) == 25 && tl_countdown_s(&tl, TL_PH_EW, now) == 29);
    assert(count_green_toggles(TL_GREEN_MS - TL_GREEN_FLASH_MS - 100, -1) == 0);      /* 前 22 秒常亮 */
    assert(tl_outputs(&tl, now) & 4u);
    assert(count_green_toggles(TL_GREEN_FLASH_MS, -1) >= 5);                          /* 最后 3 秒闪烁 */
    assert(tl.phase == TL_PH_NS && tl.state == TL_ST_GREEN);
    run(200, -1);
    assert(tl.state == TL_ST_YELLOW && tl_countdown_s(&tl, TL_PH_NS, now) == 3);
    assert(tl_outputs(&tl, now) & 2u);                                                /* 黄灯从亮开始 */
    assert(count_toggles(2u, TL_YELLOW_MS - 300, -1) >= 4);                           /* 全程闪烁 */
    assert(tl.state == TL_ST_YELLOW);
    run(300 + TL_ALLRED_MS, -1);
    assert(tl.phase == TL_PH_EW && tl.state == TL_ST_GREEN);
    run(180000, -1);                                    /* 多跑几个周期，靠不变量检查 */

    /* 红灯 30 秒：南北从黄变红到再次变绿 */
    reset();
    while (tl_light(&tl, TL_N) != TL_RED)
        run(DT, -1);
    t0 = now;
    assert(tl_countdown_s(&tl, TL_PH_NS, now) == 30);
    while (tl_light(&tl, TL_N) != TL_GREEN)
        run(DT, -1);
    assert(now - t0 + DT >= 30000 && now - t0 <= 30000 + DT);

    /* 2. 红灯早断：南北绿灯第 6 秒，西口来救护车 → 先闪 3 秒 → 黄 → 全红 → 东西绿灯保持 */
    reset();
    run(6000, -1);
    run(DT, TL_W);
    assert(tl.preempt && tl.approach == TL_W && tl.state == TL_ST_GREEN && tl.phase == TL_PH_NS);
    assert(tl_countdown_s(&tl, TL_PH_NS, now) == 3);
    assert(count_green_toggles(TL_GREEN_FLASH_MS - 100, TL_W) >= 4);
    assert(tl.state == TL_ST_GREEN);
    run(200, TL_W);
    assert(tl.state == TL_ST_YELLOW && tl.phase == TL_PH_NS);
    run(TL_YELLOW_MS + TL_ALLRED_MS, TL_W);
    assert(tl.phase == TL_PH_EW && tl.state == TL_ST_GREEN && tl_light(&tl, TL_W) == TL_GREEN);
    assert(tl_countdown_s(&tl, TL_PH_EW, now) == -1 && tl_countdown_s(&tl, TL_PH_NS, now) == -1);
    run(40000, TL_W);                                   /* 救护车 40 秒内一直绿灯，不闪 */
    assert(tl.phase == TL_PH_EW && tl.state == TL_ST_GREEN);
    assert(tl_outputs(&tl, now) & (4u << (3 * TL_W)));
    /* 警笛消失：保持 3s 后结束优先，再绿 5s（最后 3 秒闪），然后转黄 */
    run(TL_PREEMPT_HOLD_MS - 100, -1);
    assert(tl.preempt);
    run(200, -1);
    assert(!tl.preempt && tl.state == TL_ST_GREEN);
    run(TL_RELEASE_GREEN_MS - 300, -1);
    assert(tl.state == TL_ST_GREEN);
    run(400, -1);
    assert(tl.state == TL_ST_YELLOW && tl.phase == TL_PH_EW);

    /* 3. 最短绿灯：南北绿灯第 2 秒来东口救护车，绿灯到第 5 秒结束（第 2~5 秒闪烁） */
    reset();
    run(2000, -1);
    run(2900, TL_E);
    assert(tl.state == TL_ST_GREEN && tl.phase == TL_PH_NS);
    assert(tl_countdown_s(&tl, TL_PH_EW, now) == 5);    /* 0.1s + 黄 3s + 全红 1s，向上取整 */
    run(200, TL_E);
    assert(tl.state == TL_ST_YELLOW);

    /* 4. 绿灯延长：南北绿灯第 5 秒北口来救护车，超过 25 秒仍绿 */
    reset();
    run(5000, -1);
    run(30000, TL_N);
    assert(tl.phase == TL_PH_NS && tl.state == TL_ST_GREEN && tl.approach == TL_N);
    run(DT, TL_S);                                      /* 同一相位改向直接更新 */
    assert(tl.approach == TL_S && tl.state == TL_ST_GREEN);

    /* 5. 方向抖动：北/东每 0.2 秒交替，不应来回切相位 */
    reset();
    run(3000, -1);
    run(DT, TL_N);
    t0 = now;
    for (i = 0; i < 50; i++)
        run(200, (i & 1) ? TL_N : TL_E);
    assert(tl.phase == TL_PH_NS && tl.state == TL_ST_GREEN && now - t0 >= 9000);
    /* 东口持续 1 秒以上才改，改后绿灯先闪 3 秒再转黄 */
    run(TL_SWITCH_CONFIRM_MS + 100, TL_E);
    assert(tl.approach == TL_E && tl.state == TL_ST_GREEN);
    run(TL_GREEN_FLASH_MS, TL_E);
    assert(tl.state == TL_ST_YELLOW);

    /* 6. 优先放行刚结束的相位：南北黄灯中北口来车 → 全红后再放南北 */
    reset();
    run(TL_GREEN_MS + 1000, -1);                        /* 南北黄灯中 */
    assert(tl.state == TL_ST_YELLOW && tl.phase == TL_PH_NS);
    run(TL_YELLOW_MS + TL_ALLRED_MS, TL_N);
    assert(tl.phase == TL_PH_NS && tl.state == TL_ST_GREEN && tl.dur == 0);

    /* 7. 随机请求 10 分钟：只靠不变量检查 */
    reset();
    srand(1);
    for (i = 0; i < 600; i++) {
        int r = rand() % 10, req = r < 5 ? -1 : r - 5 < TL_NUM_APPROACH ? r - 5 : -1;
        run(200u + (uint32_t)(rand() % 3000), req);
    }

    puts("TRAFFIC LIGHT PASSED: cycle, green and yellow flash, early red cut, green extension, min green, release, debounce, safety invariants");
    return 0;
}
