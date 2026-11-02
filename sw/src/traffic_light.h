/*
 * 路口信号灯控制（应急车辆"声学让行"）
 *
 * 四个进口（北/南/东/西），两个相位：南北同放、东西同放。
 * 正常循环：绿 TL_GREEN_MS（最后 TL_GREEN_FLASH_MS 闪烁）→ 黄 TL_YELLOW_MS（全程闪烁）→ 全红 TL_ALLRED_MS → 另一相位绿 ……
 * 优先放行（识别到救护车且方向有效，按方位换算成来车进口）：
 *   来车相位正是绿灯：保持绿灯（绿灯延长）；
 *   来车相位是红灯：当前绿灯提前结束（红灯早断），至少放满 TL_MIN_GREEN_MS，结束前同样闪烁，
 *   再经黄灯、全红后来车相位绿灯并保持；
 *   警笛消失 TL_PREEMPT_HOLD_MS 后结束优先，绿灯再放 TL_RELEASE_GREEN_MS 后恢复正常循环。
 * 黄灯、全红从不跳过，任何时刻最多一个相位不是红灯。
 * 纯 C，不依赖硬件（PC 测试 sw/test/test_traffic.c）。
 */
#ifndef TRAFFIC_LIGHT_H
#define TRAFFIC_LIGHT_H

#include <stdint.h>

/* 配时（毫秒）：每个方向绿 25s、黄 3s、红 30s。红灯时长 = 全红 + 另一方向绿 + 黄 + 全红，由下面几项决定 */
#define TL_GREEN_MS          25000u
#define TL_GREEN_FLASH_MS     3000u     /* 绿灯最后这段时间闪烁（亮灭各 0.5s） */
#define TL_YELLOW_MS          3000u
#define TL_ALLRED_MS          1000u
#define TL_MIN_GREEN_MS       5000u     /* 红灯早断前，当前绿灯至少放行这么久（行人、已进入路口的车） */
#define TL_RELEASE_GREEN_MS   5000u     /* 优先结束后，来车相位绿灯再放这么久 */
#define TL_PREEMPT_HOLD_MS    3000u     /* 警笛方向消失这么久才结束优先（识别偶尔中断不致来回切换） */
#define TL_SWITCH_CONFIRM_MS  1000u     /* 优先期间来车相位改变，须持续这么久才改 */

/* 阵列 0°（0 号麦克风）指向的路口方向；方位逆时针为正，所以 90° 为西、180° 为南、270° 为东 */
#ifndef TL_NORTH_AZ_DEG
#define TL_NORTH_AZ_DEG       0.0f
#endif

enum { TL_N = 0, TL_S, TL_E, TL_W, TL_NUM_APPROACH };
enum { TL_PH_NS = 0, TL_PH_EW = 1 };
enum { TL_ST_GREEN = 0, TL_ST_YELLOW, TL_ST_ALLRED };
enum { TL_RED = 0, TL_YELLOW, TL_GREEN };

/* 输出位：进口 a 的红/黄/绿为 bit 3a / 3a+1 / 3a+2（与 vivado 中 tl_out 一致；bit 12 未使用，恒为 0） */

typedef struct {
    int      phase;             /* 当前（绿/黄）或刚结束（全红）的相位 */
    int      state;             /* TL_ST_* */
    uint32_t t_state;           /* 进入当前状态的时刻 */
    uint32_t dur;               /* 当前状态时长；绿灯保持（优先）时为 0 */
    int      preempt;           /* 正在优先放行 */
    int      approach;          /* 优先的来车进口 TL_N..，无为 -1 */
    uint32_t last_req;          /* 最近一次收到优先请求的时刻 */
    int      pend_approach;     /* 优先期间新请求的进口（相位不同时须持续确认） */
    uint32_t pend_since;
} tl_state_t;

void tl_init(tl_state_t *tl, uint32_t now_ms);
/* 每块调用：req_approach 为本块的来车进口（TL_N..），没有请求为 -1。返回 1 表示状态有变化 */
int  tl_step(tl_state_t *tl, uint32_t now_ms, int req_approach);
uint32_t tl_outputs(const tl_state_t *tl, uint32_t now_ms);
int  tl_light(const tl_state_t *tl, int approach);            /* TL_RED / TL_YELLOW / TL_GREEN */
/* 相位 ph 当前灯色剩余秒数（向上取整），优先保持中（不定时）返回 -1 */
int  tl_countdown_s(const tl_state_t *tl, int ph, uint32_t now_ms);
int  tl_phase_of(int approach);
int  tl_approach_from_az(float az_deg);
const char *tl_approach_name(int approach);                    /* "北" "南" "东" "西" */

#endif
