/*
 * HDMI 帧缓冲同步：PS 在 DDR 中画好整屏（shadow），只把与 PL 帧缓冲不同的字写过去，
 * 每次调用最多写 HDMI_SYNC_MAX_WORDS 个字，避免占用定位主循环的时间预算。
 */
#ifndef HDMI_OUT_H
#define HDMI_OUT_H

#include "hdmi_ui.h"

#define HDMI_FB_BASEADDR    0x43C00000u     /* 由 vivado/create_project.tcl 固定分配 */
#define HDMI_STATUS_OFFSET  0x3FFF0u        /* bit0 = HPD，bit31:8 = 帧计数 */
#define HDMI_SYNC_MAX_WORDS 6000            /* 每次最多写入的字数（约 1ms） */

void     hdmi_out_init(void);
/* 1：显示器已连接（HPD） */
int      hdmi_out_connected(void);
uint32_t hdmi_out_frame_count(void);
/* 把 shadow 中有变化的部分写入 PL；返回 1 表示已全部同步 */
int      hdmi_out_sync(const hdmi_fb_t shadow);

#endif
