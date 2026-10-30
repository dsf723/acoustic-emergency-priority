/*
 * 显示用方向滤波：在最近一小段时间内的检测结果中做加权投票，
 * 取权重和最大的方向簇，再求该簇的加权圆周平均。
 * 偶发的离群结果（例如墙面反射导致的反方向）权重不足，不会让显示跳变。
 * 不依赖硬件，可在 PC 上测试。
 */
#ifndef DIR_FILTER_H
#define DIR_FILTER_H

#include <stdint.h>

#define DIR_FILTER_SIZE         16      /* 保留的最近检测数 */
#define DIR_FILTER_WINDOW       36      /* 参与投票的时间窗（块数，约 0.75s） */
#define DIR_FILTER_CLUSTER_DEG  20.0f   /* 同一方向簇的角度范围（±） */
#define DIR_FILTER_WEIGHT_MAX   25.0f   /* 防止单块异常权重压倒整簇 */
#define DIR_FILTER_JUMP_DEG     60.0f
#define DIR_FILTER_CONFIRM_N    5       /* 大跳变至少来自 5 个不同块 */
#define DIR_FILTER_CONFIRM_SPAN 8       /* 首尾跨度至少约 168ms */
#define DIR_FILTER_CONFIRM_WIN  18      /* 约 0.38s 内的支持证据 */
#define DIR_FILTER_SWITCH_RATIO 1.25f

void  dir_filter_reset(void);
/* 连续警笛使用大角度切换确认；其他声音保持原有响应。 */
void  dir_filter_set_guard(int active);
/* 加入一次检测：方位角（度）、权重（>0）、块序号 */
void  dir_filter_add(float az_deg, float weight, uint32_t blk);
/* 返回块 blk 时刻的滤波方向（度）；窗口内没有检测时返回 -1 */
float dir_filter_get(uint32_t blk);

#endif
