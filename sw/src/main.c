/*
 * 启明星 ZYNQ7020 + Sipeed 6+1 麦克风阵列 声源定位（裸机）
 *
 * 数据流：PL mic_array_i2s_rx --AXIS--> AXI DMA(S2MM) --> DDR 双缓冲 --> doa_process()
 * 识别  ：当前部署仅输出规则警笛候选；尚未验证能区分救护车/警车/消防车。
 * 抗干扰：siren_focus 按方向分开跟踪音调分量，方向/远近只取"听起来像警笛"的那个方向（见 siren_focus.c）。
 * 输出  ：确认警笛后才输出角度/远近；方向不足显示UNK、灯灭；普通声音保持LISTENING。
 *
 * 串口输出格式（115200 8N1）：
 *   DOA,<角度 x10>,<能量-噪声 dB x10>,<峰值比 x100>,<滤波后角度 x10>   目标方向可用时节流输出
 *   EVT,SIREN,<方位 x10或-10>,<NEAR|MID|FAR|UNKNOWN>               警笛开始；-10表示方向未确认
 *   EVT,SIREN_END                                                      警笛结束
 *   RNG,<SOUND|CLAP|SIREN|NONE>,<NEAR|MID|FAR|UNKNOWN>                  每秒一次远近状态
 *   DST,<估计距离 米x100 或 -1>,<1 米处参考音量 dB x10>                每秒一次（按警笛音量估距离）
 *   CAL,OK,<参考音量 x10> / CAL,FAIL,NO_SIREN                          串口发 'C' 标定后的回应
 *   TL,<NS|EW>,<GREEN|YELLOW|ALLRED>,<NORMAL|PRIORITY_N/S/E/W>        信号灯状态变化时
 * 串口输入：'C' = 把当前警笛音量标定为 1 米处（喇叭放在 1 米处、警笛响着时发送）
 * 路口信号灯：tl_out 13 路（J4.12~24，见 constr/sound_loc.xdc）
 * 4 块 OLED 代替数码管，每个进口一块、各用一条 I2C（见 oled_ssd1306.h）
 *   LVL,<ch0>,...,<ch7>,<处理耗时 us>,<overrun>                         每秒一行，各通道电平 dBFS
 *   CAP,<seq_errors>,<short_packets>,<dma_errors>                       累计采集错误计数
 *   FEA,...（调试器把 debug_fea 写 1 时每块输出特征，用于标定，字段见代码）
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "xparameters.h"
#include "xaxidma.h"
#include "xgpio.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "xuartps_hw.h"
#include "doa.h"
#include "doa_config.h"
#include "sk9822.h"
#include "oled_ui.h"
#include "oled_ssd1306.h"
#include "hdmi_out.h"
#include "dir_filter.h"
#include "sound_class.h"
#include "target_output.h"
#include "siren_focus.h"
#include "traffic_light.h"
#include "oled_tl.h"

#define PKT_WORDS   (DOA_FRAMES_PER_PKT * 8)
#define PKT_BYTES   (PKT_WORDS * 4)
#define BLOCKS_PER_SEC  ((int)(DOA_FS_HZ / DOA_FRAMES_PER_PKT + 0.5f))

static uint32_t dma_buf[2][PKT_WORDS] __attribute__((aligned(64)));

#if DEBUG_RECORD_BLOCKS > 0
/*
 * 调试录音（由 PC 端 tools/rec_dump.py 经 JTAG 控制与读出，用于离线分析与标定）：
 *   rec_mode = 0：只保存"响亮"的块（频带能量高于背景 DEBUG_RECORD_MIN_SNR_DB），循环覆盖
 *   rec_mode = 1：待触发连续录音。连续循环录制，第一个响亮块出现后再录
 *                 DEBUG_RECORD_BLOCKS - DEBUG_RECORD_PREROLL 块即停止（保留触发前约 1 秒）
 * rec_freeze 由调试器写 1 暂停录音（读出期间保持数据不变），写 0 恢复；
 * 调试器把 rec_idx 写 0 即清空录音。
 */
uint32_t rec_buf[DEBUG_RECORD_BLOCKS][PKT_WORDS] __attribute__((aligned(64)));
uint32_t rec_blkno[DEBUG_RECORD_BLOCKS] __attribute__((aligned(64)));
volatile uint32_t rec_idx;          /* 已写入的总块数，下一块写入 rec_buf[rec_idx % N] */
volatile uint32_t rec_freeze;
volatile uint32_t rec_mode;
volatile int32_t  rec_left = -1;    /* rec_mode 1：触发后剩余块数，-1 表示尚未触发 */
volatile float    rec_trig_db = 25.0f;  /* rec_mode 1：触发门限（高于背景的 dB 数），调试器可改 */
#endif
static uint32_t blk_no;             /* 自启动以来处理的块序号 */
volatile uint32_t debug_fea;        /* 调试器写 1：每块串口输出特征（FEA 行），用于标定 */
/*
 * 全链路计时（tools/chain_test.py 经 JTAG 读写）：调试器写 ct_arm = 1 开始一次测量，
 * 之后各事件第一次发生时记下块号（与 rec_blkno 同一计数）和毫秒时刻：
 *   0 声音出现（高于背景 CT_SOUND_DB）  1 警笛确认  2 方向确认  3 信号灯进入优先  4 来车进口变绿
 * ct_info：方向确认时的方位 x10、换算的进口、声音出现时的相位与灯色（phase << 8 | state）
 */
#define CT_SOUND_DB 15.0f
volatile uint32_t ct_arm;
volatile uint32_t ct_blk[5], ct_ms[5];
volatile int32_t  ct_info[3];
volatile uint32_t capture_seq_errors;
volatile uint32_t capture_short_packets;
volatile uint32_t capture_dma_errors;

static XAxiDma dma;
static XGpio   gpio_status;         /* 通道 1：frame_cnt，通道 2：overrun */
static XGpio   gpio_led;
static XGpio   gpio_tl;             /* 路口信号灯 13 路输出（tl_out，接 J4.12~24） */
static tl_state_t tl;               /* 信号灯状态机：正常循环 + 救护车优先放行 */
static oled_fb_t oled_fb[OLED_NUM];       /* 下标为进口 TL_N/S/E/W */
static hdmi_fb_t hdmi_shadow;       /* HDMI 整屏画面（DDR），增量同步到 PL 帧缓冲 */
#define HDMI_RENDER_BLOCKS  6       /* 每 6 块（约 0.13s）重画一次 HDMI 画面 */
#define OLED_BLOCK_BUDGET_US 5000u   /* 每个音频块（21ms）留给 OLED 发送的时间 */
#define OLED_MAINT_BLOCKS    96      /* 约每 2s：重试掉线的屏，并轮流重新初始化一块在线的屏（约 8s 一轮） */
static char hdmi_mode_text[64];      /* HDMI 底栏右侧：信号灯状态 */
static int fault_screen_shown;
/* 灯模块低电平点亮时改为 0x0FFF */
#define TL_OUT_INVERT       0x0000u

static uint32_t now_ms(void)
{
    XTime t;
    XTime_GetTime(&t);
    return (uint32_t)(t / (COUNTS_PER_SECOND / 1000u));
}

static void put_num(char *buf, int v)
{
    char t[8];
    int n = 0;
    if (v < 0) {
        strcat(buf, "--");
        return;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    buf += strlen(buf);
    while (n)
        *buf++ = t[--n];
    *buf = '\0';
}

/* HDMI 底栏文字："南北 绿 18 · 东西 红 23" 或 "救护车优先 · 放行西口" */
static void tl_text(char *buf, uint32_t now)
{
    static const char *lname[3] = { "红", "黄", "绿" };
    if (tl.preempt) {
        strcpy(buf, "救护车优先 · 放行");
        strcat(buf, tl_approach_name(tl.approach));
        strcat(buf, "口");
        return;
    }
    strcpy(buf, "南北 ");
    strcat(buf, lname[tl_light(&tl, TL_N)]);
    strcat(buf, " ");
    put_num(buf, tl_countdown_s(&tl, TL_PH_NS, now));
    strcat(buf, " · 东西 ");
    strcat(buf, lname[tl_light(&tl, TL_E)]);
    strcat(buf, " ");
    put_num(buf, tl_countdown_s(&tl, TL_PH_EW, now));
}

static void tl_write(void)
{
    XGpio_DiscreteWrite(&gpio_tl, 1, tl_outputs(&tl, now_ms()) ^ TL_OUT_INVERT);
}

/* 画面内容（取整后的角度等）是否与上次绘制的不同；不同才重画，节省 CPU */
static int hdmi_ui_changed(const oled_ui_state_t *a, const oled_ui_state_t *b)
{
    int alarm_a = a->has_sound && a->cls_name && a->cls_name[0];
    int alarm_b = b->has_sound && b->cls_name && b->cls_name[0];
    return alarm_a != alarm_b || a->direction_valid != b->direction_valid ||
           a->band != b->band || lroundf(a->azimuth_deg) != lroundf(b->azimuth_deg);
}

static int dma_init(void)
{
    XAxiDma_Config *cfg = XAxiDma_LookupConfig(XPAR_AXI_DMA_0_DEVICE_ID);
    if (!cfg || XAxiDma_CfgInitialize(&dma, cfg) != XST_SUCCESS)
        return -1;
    if (XAxiDma_HasSg(&dma))
        return -1;
    XAxiDma_IntrDisable(&dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    return 0;
}

static int dma_start(int idx)
{
    /* 先作废缓存，防止脏缓存行在 DMA 写入后被写回覆盖数据 */
    Xil_DCacheInvalidateRange((INTPTR)dma_buf[idx], PKT_BYTES);
    return XAxiDma_SimpleTransfer(&dma, (UINTPTR)dma_buf[idx], PKT_BYTES,
                                  XAXIDMA_DEVICE_TO_DMA);
}

/* 等待传输完成，返回实际接收字节数；DMA 出错返回 -1 */
static int dma_wait(void)
{
    while (XAxiDma_Busy(&dma, XAXIDMA_DEVICE_TO_DMA)) {
        u32 sr = XAxiDma_ReadReg(dma.RegBase, XAXIDMA_RX_OFFSET + XAXIDMA_SR_OFFSET);
        if (sr & XAXIDMA_ERR_ALL_MASK)
            return -1;
    }
    return (int)XAxiDma_ReadReg(dma.RegBase, XAXIDMA_RX_OFFSET + XAXIDMA_BUFFLEN_OFFSET);
}

/* xil_printf 不支持浮点，按 x10 / x100 定点打印 */
static int fx(float v, float scale)
{
    float s = v * scale;
    return (int)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

/* 重画 4 块屏的倒计时并整屏发送（阻塞，只用于启动和故障） */
static void oled_show_all(void)
{
    int n;
    for (n = 0; n < OLED_NUM; n++) {
        oled_tl_render(oled_fb[n], tl_countdown_s(&tl, tl_phase_of(n), now_ms()));
        oled_flush(n, oled_fb[n]);
    }
}

static void invalidate_target(snd_state_t *sc, target_output_t *target,
                              oled_ui_state_t *ui, int *siren_prev)
{
    if (*siren_prev) xil_printf("EVT,SIREN_END\r\n");
    *siren_prev = 0;
    sc_init();
    doa_noise_freeze(0);
    target_output_reset();
    siren_focus_reset();
    memset(sc, 0, sizeof(*sc));
    memset(target, 0, sizeof(*target));
    memset(ui, 0, sizeof(*ui));
    sk9822_show_direction(-1.0f);
    /* Error branches can keep skipping the normal UI loop. Clear the panel
     * once on entering a fault so it cannot indefinitely show an old target. */
    if (!fault_screen_shown && oled_available())
        oled_show_all();
    if (!fault_screen_shown)
        hdmi_ui_render(hdmi_shadow, ui, hdmi_mode_text);     /* 由主循环逐步同步到屏幕 */
    fault_screen_shown = 1;
}

int main(void)
{
    doa_result_t res;
    int cur = 0;
    int blk = 0;
    u32 proc_us = 0;
    int i;
    int oled_next = 0;
    int oled_keep = 0;
    int oled_maint_cnt = 0;
    int oled_last[OLED_NUM] = { -2, -2, -2, -2 };   /* 已画的倒计时 */
    int hdmi_synced = 1;
    uint32_t hdmi_last_render = 0;
    int siren_prev = 0;
    snd_state_t sc = { 0 };
    target_output_t target = { 0 };
    siren_focus_info_t sfi;
    oled_ui_state_t ui = { 0, 0.0f, "", OLED_BAND_NONE, 0 };
    oled_ui_state_t hdmi_ui_last;

    xil_printf("\r\n=== Sound source localization: ZYNQ7020 + Sipeed 6+1 mic array ===\r\n");
    xil_printf("MODE,SIREN_ONLY_RULE_BASELINE,NOT_AMBULANCE_VERIFIED\r\n");
    xil_printf("fs=%d Hz, %d frames/packet, radius=%d mm\r\n",
               (int)DOA_FS_HZ, DOA_FRAMES_PER_PKT, fx(DOA_ARRAY_RADIUS_M, 1000.0f));

    XGpio_Initialize(&gpio_status, XPAR_AXI_GPIO_STATUS_DEVICE_ID);
    XGpio_Initialize(&gpio_led, XPAR_AXI_GPIO_LED_DEVICE_ID);
    sk9822_init(&gpio_led);
    sk9822_show_direction(-1.0f);

    /* 路口信号灯：上电从南北绿灯开始正常循环 */
    XGpio_Initialize(&gpio_tl, XPAR_AXI_GPIO_TL_DEVICE_ID);
    XGpio_SetDataDirection(&gpio_tl, 1, 0);
    tl_init(&tl, now_ms());
    tl_write();
    tl_text(hdmi_mode_text, now_ms());
    xil_printf("TL,init,north_az=%d\r\n", fx(TL_NORTH_AZ_DEG, 1.0f));

    oled_init();
    oled_show_all();
    for (i = 0; i < OLED_NUM; i++) {
        static const char *const oled_name[OLED_NUM] = { "N", "S", "E", "W" };
        if (oled_ok(i))
            xil_printf("OLED %s ready\r\n", oled_name[i]);
        else
            xil_printf("OLED %s not found (addr 0x3C), continuing without it\r\n", oled_name[i]);
    }

    /* HDMI：640x480，先画初始画面并整屏写入 */
    hdmi_out_init();
    {
        XTime ta, tb, tc;
        XTime_GetTime(&ta);
        hdmi_ui_render(hdmi_shadow, &ui, hdmi_mode_text);        /* 第一次含静态背景 */
        XTime_GetTime(&tb);
        hdmi_ui_render(hdmi_shadow, &ui, hdmi_mode_text);
        XTime_GetTime(&tc);
        xil_printf("HDMI UI render: first %d us, update %d us\r\n",
                   (int)((tb - ta) / (COUNTS_PER_SECOND / 1000000)),
                   (int)((tc - tb) / (COUNTS_PER_SECOND / 1000000)));
    }
    hdmi_ui_last = ui;
    while (!hdmi_out_sync(hdmi_shadow))
        ;
    {
        u32 fc0 = hdmi_out_frame_count();
        for (i = 0; i < 2000000; i++)
            __asm__ volatile ("nop");
        xil_printf("HDMI %s, video frames %d -> %d (should increase)\r\n",
                   hdmi_out_connected() ? "monitor connected (HPD=1)" : "no monitor detected (HPD=0)",
                   (int)fc0, (int)hdmi_out_frame_count());
    }

    if (dma_init() != 0) {
        xil_printf("AXI DMA init failed\r\n");
        return -1;
    }
    {
        u32 f0 = XGpio_DiscreteRead(&gpio_status, 1);
        for (i = 0; i < 2000000; i++)
            __asm__ volatile ("nop");
        xil_printf("PL frame counter: %d -> %d (should increase)\r\n",
                   (int)f0, (int)XGpio_DiscreteRead(&gpio_status, 1));
    }

    doa_init();
    sc_init();
    target_output_reset();
    siren_focus_reset();
    dma_start(cur);

    while (1) {
        int n = dma_wait();
        XTime t0, t1;
        int ret;
        int telemetry_block = blk + 1 >= BLOCKS_PER_SEC;
        int feature_block = 0;

        if (n < 0) {
            capture_dma_errors++;
            invalidate_target(&sc, &target, &ui, &siren_prev);
            xil_printf("DMA error, reset\r\n");
            XAxiDma_Reset(&dma);
            while (!XAxiDma_ResetIsDone(&dma))
                ;
            dma_init();
            dma_start(cur);
            continue;
        }
        dma_start(cur ^ 1);                     /* 立即启动下一块接收，与计算并行 */
        Xil_DCacheInvalidateRange((INTPTR)dma_buf[cur], PKT_BYTES);

        if (n != PKT_BYTES) {
            capture_short_packets++;
            invalidate_target(&sc, &target, &ui, &siren_prev);
            xil_printf("packet length %d != %d, check FRAMES_PER_PKT\r\n", n, PKT_BYTES);
            cur ^= 1;
            continue;
        }

        XTime_GetTime(&t0);
        ret = doa_process(dma_buf[cur], DOA_FRAMES_PER_PKT, &res);
        /* 方向、远近只取像警笛的那个方向的分量（不被更响的干扰源拉走）；耗时计入 proc_us */
        if (ret == 0)
            siren_focus_apply(&res, siren_prev, &sfi);
        XTime_GetTime(&t1);
        proc_us = (u32)((t1 - t0) / (COUNTS_PER_SECOND / 1000000));

#if DEBUG_RECORD_BLOCKS > 0
        if (rec_mode == 1 && !rec_freeze && ret == 0 && rec_left < 0 &&
            res.energy_db - res.noise_db > rec_trig_db)
            rec_left = DEBUG_RECORD_BLOCKS - DEBUG_RECORD_PREROLL;    /* 触发：再录满缓冲区后停止 */
        if (!rec_freeze && ret == 0 &&
            (rec_mode == 1 || res.energy_db - res.noise_db > DEBUG_RECORD_MIN_SNR_DB)) {
            uint32_t slot = rec_idx % DEBUG_RECORD_BLOCKS;
            memcpy(rec_buf[slot], dma_buf[cur], PKT_BYTES);
            rec_blkno[slot] = blk_no;
            /* 写回 DDR，JTAG 才能读到 */
            Xil_DCacheFlushRange((INTPTR)rec_buf[slot], PKT_BYTES);
            Xil_DCacheFlushRange((INTPTR)&rec_blkno[slot], sizeof(uint32_t));
            rec_idx++;
            if (rec_mode == 1 && rec_left >= 0 && --rec_left <= 0)
                rec_freeze = 1;
        }
#endif
        blk_no++;

        if (ret == 0) {
            fault_screen_shown = 0;
            /* 声音识别（拍手 / 警笛）与距离；警笛持续期间冻结背景噪声估计 */
            sc_process(&res, blk_no, &sc);
            doa_noise_freeze(sc.siren_active);
            target_output_update(&res, &sc, blk_no, &target);

            /* 串口 115200 下一行约 6ms，只输出有声音的块，避免拖慢主循环 */
            feature_block = debug_fea && blk_no % 4 == 0 &&
                            res.energy_db - res.noise_db > 6.0f;
            if (feature_block)
                xil_printf("FEA,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\r\n", (int)blk_no,
                           fx(res.energy_db, 10.0f), fx(res.noise_db, 10.0f),
                           fx(res.peak_level_db, 10.0f), fx(res.peak_ratio, 100.0f),
                           fx(res.coherence, 100.0f), fx(res.flatness, 1000.0f),
                           fx(res.centroid_hz, 1.0f), fx(res.peak_hz, 1.0f),
                           fx(res.tonality_db, 10.0f), fx(res.azimuth_deg, 10.0f),
                           res.valid, sc.cls);

            /* Only confirmed siren observations may reach target direction history.
             * Throttle UART to protect the 21ms DMA processing budget. */
            if (target.direction_valid && sc.dir_ok && (blk_no % 4 == 0) &&
                !telemetry_block && !feature_block && !sc.event) {
                xil_printf("DOA,%d,%d,%d,%d\r\n", fx(res.azimuth_deg, 10.0f),
                           fx(res.energy_db - res.noise_db, 10.0f), fx(res.peak_ratio, 100.0f),
                           fx(target.azimuth_deg, 10.0f));
            }

            if (sc.event && target.active)
                xil_printf("EVT,SIREN,%d,%s\r\n",
                           target.direction_valid ? fx(target.azimuth_deg, 10.0f) : -10,
                           sc_band_name(target.band));
            if (siren_prev && !sc.siren_active)
                xil_printf("EVT,SIREN_END\r\n");
            siren_prev = sc.siren_active;
        }

        if (ret != 0) {
            capture_seq_errors++;
            invalidate_target(&sc, &target, &ui, &siren_prev);
        }
        /* Class may be valid while direction is unknown; never retain an old pointer. */
        ui.has_sound = target.active;
        ui.direction_valid = target.direction_valid;
        ui.azimuth_deg = target.azimuth_deg;
        ui.cls_name = target.active ? "SIREN" : "";
        ui.band = target.band;
        ui.distance_m = (target.active && target.direction_valid && target.band) ? sc.distance_m : -1.0f;

        /* 串口命令：'C' = 把当前警笛的音量标定为 1 米处（喇叭放在阵列 1 米处、警笛响着时发送） */
        if (XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
            u8 cmd = XUartPs_RecvByte(STDIN_BASEADDRESS);
            if (cmd == 'C' || cmd == 'c') {
                if (sc_range_calibrate(1.0f))
                    xil_printf("CAL,OK,%d\r\n", fx(sc_range_ref_db(), 10.0f));
                else
                    xil_printf("CAL,FAIL,NO_SIREN\r\n");
            }
        }
        if (target.active && target.direction_valid) {
            sk9822_show_direction_rgb(ui.azimuth_deg, 255, 0, 0);
        } else {
            sk9822_show_direction(-1.0f);
        }

        /* 路口信号灯：确认的救护车方向换算成来车进口，请求优先放行 */
        {
            uint32_t now = now_ms();
            int req = (target.active && target.direction_valid) ? tl_approach_from_az(target.azimuth_deg) : -1;
            char txt[64];
            if (tl_step(&tl, now, req))
                xil_printf("TL,%s,%s,%s%s\r\n", tl.phase == TL_PH_NS ? "NS" : "EW",
                           tl.state == TL_ST_GREEN ? "GREEN" : tl.state == TL_ST_YELLOW ? "YELLOW" : "ALLRED",
                           tl.preempt ? "PRIORITY_" : "NORMAL",
                           tl.preempt ? (tl.approach == TL_N ? "N" : tl.approach == TL_S ? "S" :
                                         tl.approach == TL_E ? "E" : "W") : "");
            tl_write();
            if (ct_arm == 1) {
                for (i = 0; i < 5; i++)
                    ct_blk[i] = ct_ms[i] = 0;
                ct_arm = 2;
            }
            if (ct_arm == 2) {
                int ev[5];
                ev[0] = ret == 0 && res.energy_db - res.noise_db > CT_SOUND_DB;
                ev[1] = target.active;
                ev[2] = req >= 0;
                ev[3] = tl.preempt;
                ev[4] = tl.preempt && tl.state == TL_ST_GREEN &&
                        tl.phase == ((tl.approach == TL_N || tl.approach == TL_S) ? TL_PH_NS : TL_PH_EW);
                for (i = 0; i < 5; i++) {
                    if (ev[i] && !ct_blk[i] && (i == 0 || ct_blk[0])) {
                        ct_blk[i] = blk_no;
                        ct_ms[i] = now;
                        if (i == 0)
                            ct_info[2] = tl.phase << 8 | tl.state;
                        if (i == 2) {
                            ct_info[0] = fx(target.azimuth_deg, 10.0f);
                            ct_info[1] = req;
                        }
                    }
                }
                if (ct_blk[4])
                    ct_arm = 3;             /* 测完：保持结果供读出 */
            }
            tl_text(txt, now);
            if (strcmp(txt, hdmi_mode_text) != 0) {
                strcpy(hdmi_mode_text, txt);
                hdmi_ui_last.has_sound = -1;        /* 倒计时变了：强制重画 HDMI */
            }
        }

        /* HDMI：内容变化且上一画面已同步完时重画（最快每 HDMI_RENDER_BLOCKS 块一次），每块最多写约 1ms */
        if (hdmi_synced && blk_no - hdmi_last_render >= HDMI_RENDER_BLOCKS &&
            (hdmi_ui_changed(&ui, &hdmi_ui_last) || hdmi_ui_last.has_sound < 0)) {
            hdmi_ui_render(hdmi_shadow, &ui, hdmi_mode_text);
            hdmi_ui_last = ui;
            hdmi_last_render = blk_no;
        }
        hdmi_synced = hdmi_out_sync(hdmi_shadow);

        /*
         * OLED 代替数码管：倒计时变化时重画；4 块屏轮流发送有变化的列，
         * 每个音频块最多占用 OLED_BLOCK_BUDGET_US，数字换一次通常约 0.15s 刷完
         */
        if (!telemetry_block && !feature_block) {
            uint32_t now = now_ms();
            XTime ts, te;
            int n, idle = 0;
            for (n = 0; n < OLED_NUM; n++) {
                int secs = tl_countdown_s(&tl, tl_phase_of(n), now);
                if (secs != oled_last[n]) {
                    oled_tl_render(oled_fb[n], secs);
                    oled_last[n] = secs;
                }
            }
            if (++oled_maint_cnt >= OLED_MAINT_BLOCKS) {
                oled_maint_cnt = 0;
                for (n = 0; n < OLED_NUM; n++)
                    if (!oled_ok(n))
                        oled_maintain(n, oled_fb[n]);
                if (oled_ok(oled_keep))
                    oled_maintain(oled_keep, oled_fb[oled_keep]);
                oled_keep = (oled_keep + 1) % OLED_NUM;
            }
            XTime_GetTime(&ts);
            do {
                idle = oled_sync_step(oled_next, oled_fb[oled_next]) ? 0 : idle + 1;
                oled_next = (oled_next + 1) % OLED_NUM;
                XTime_GetTime(&te);
            } while (idle < OLED_NUM &&
                     (te - ts) < (XTime)OLED_BLOCK_BUDGET_US * (COUNTS_PER_SECOND / 1000000));
        }

        if (ret != 0)
            xil_printf("packet sequence error\r\n");

        if (++blk >= BLOCKS_PER_SEC) {
            blk = 0;
            xil_printf("LVL");
            for (i = 0; i < 8; i++)
                xil_printf(",%d", fx(res.ch_level_db[i], 1.0f));
            xil_printf(",%d,%d\r\n", (int)proc_us, (int)(XGpio_DiscreteRead(&gpio_status, 2) & 1));
            xil_printf("RNG,%s,%s\r\n", target.active ? "SIREN" : "NONE",
                       sc_band_name(target.band));
            xil_printf("DST,%d,%d\r\n", target.active && sc.distance_m > 0.0f ? fx(sc.distance_m, 100.0f) : -1,
                       fx(sc_range_ref_db(), 10.0f));
            xil_printf("CAP,%d,%d,%d\r\n", (int)capture_seq_errors,
                       (int)capture_short_packets, (int)capture_dma_errors);
        }
        cur ^= 1;
    }
    return 0;
}
