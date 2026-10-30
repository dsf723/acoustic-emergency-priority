#include "hdmi_out.h"
#include "xil_io.h"

static hdmi_fb_t sent;          /* PL 帧缓冲当前内容的副本 */
static int       pos;           /* 下次比较的起始字 */

void hdmi_out_init(void)
{
    int i;
    /* 上电后 PL 帧缓冲为全 0；副本设为不可能的值，保证第一次同步写满整屏 */
    for (i = 0; i < HDMI_WORDS; i++)
        sent[i] = 0xFFFFFFFFu;
    pos = 0;
}

int hdmi_out_connected(void)
{
    return Xil_In32(HDMI_FB_BASEADDR + HDMI_STATUS_OFFSET) & 1u;
}

uint32_t hdmi_out_frame_count(void)
{
    return Xil_In32(HDMI_FB_BASEADDR + HDMI_STATUS_OFFSET) >> 8;
}

int hdmi_out_sync(const hdmi_fb_t shadow)
{
    int checked, written = 0;

    for (checked = 0; checked < HDMI_WORDS; checked++) {
        if (shadow[pos] != sent[pos]) {
            if (written >= HDMI_SYNC_MAX_WORDS)
                return 0;                       /* 本次额度用完，下次从这里继续 */
            Xil_Out32(HDMI_FB_BASEADDR + (uint32_t)pos * 4u, shadow[pos]);
            sent[pos] = shadow[pos];
            written++;
        }
        if (++pos == HDMI_WORDS)
            pos = 0;
    }
    return 1;
}
