/* NimbusOS - SD card on the Pi's SD host controller (EMMC2 on the Pi 4 /
 * 400, the Arasan EMMC on the Pi 3), polled, one 512-byte block at a time.
 * The controller is a standard SDHCI: the card is re-identified from
 * scratch (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD7) and switched to a 4-bit
 * bus at 25 MHz.  References: SD Host Controller Simplified Specification
 * 3.00, SD Physical Layer Simplified Specification. */
#include "arch/arm64/pi.h"
#include "drivers/pit.h"
#include "kernel/kprintf.h"

#define R_BLKSIZECNT 0x04
#define R_ARG1       0x08
#define R_CMDTM      0x0C
#define R_RESP0      0x10
#define R_DATA       0x20
#define R_STATUS     0x24
#define R_CONTROL0   0x28
#define R_CONTROL1   0x2C
#define R_INTERRUPT  0x30
#define R_IRPT_MASK  0x34
#define R_IRPT_EN    0x38
#define R_CAPS       0x40
#define R_VERSION    0xFC

#define INT_CMD_DONE  (1u << 0)
#define INT_DATA_DONE (1u << 1)
#define INT_WRITE_RDY (1u << 4)
#define INT_READ_RDY  (1u << 5)
#define INT_ERROR     (1u << 15)

#define RESP_NONE  0
#define RESP_136   (1u << 16)
#define RESP_48    (2u << 16)
#define RESP_48B   (3u << 16)
#define CRC_CHK    (1u << 19)
#define IDX_CHK    (1u << 20)
#define DATA       (1u << 21)
#define READ_DIR   (1u << 4)

#define R1  (RESP_48 | CRC_CHK | IDX_CHK)
#define R1B (RESP_48B | CRC_CHK | IDX_CHK)
#define R2  (RESP_136 | CRC_CHK)
#define R3  (RESP_48)
#define R6  (RESP_48 | CRC_CHK | IDX_CHK)
#define R7  (RESP_48 | CRC_CHK | IDX_CHK)

static uptr base;
static u32 rca, base_khz;
static bool sdhc, ok;

static inline u32 rd(u32 r) { return mmio_r(base + r); }
static inline void wr(u32 r, u32 v) { mmio_w(base + r, v); }

static bool wait_int(u32 mask, u32 ms)
{
    u64 end = timer_ticks() + ms;
    for (;;) {
        u32 i = rd(R_INTERRUPT);
        if (i & INT_ERROR) { klog("[sd  ] error interrupt %08x\n", i); wr(R_INTERRUPT, i); return false; }
        if ((i & mask) == mask) { wr(R_INTERRUPT, mask); return true; }
        if (timer_ticks() > end) { klog("[sd  ] timeout waiting for %08x (int %08x)\n", mask, i); return false; }
    }
}

static bool cmd(u32 idx, u32 arg, u32 flags, u32 *resp)
{
    u64 end = timer_ticks() + 100;
    while (rd(R_STATUS) & 3) if (timer_ticks() > end) { klog("[sd  ] CMD%u: busy\n", idx); return false; }
    wr(R_INTERRUPT, 0xFFFFFFFF);
    wr(R_ARG1, arg);
    wr(R_CMDTM, (idx << 24) | flags);
    if (!wait_int(INT_CMD_DONE, 500)) { klog("[sd  ] CMD%u failed\n", idx); return false; }
    if (resp) *resp = rd(R_RESP0);
    if ((flags & 0x30000) == RESP_48B && !(flags & DATA)) wait_int(INT_DATA_DONE, 500);
    return true;
}

static bool app_cmd(u32 idx, u32 arg, u32 flags, u32 *resp)
{
    return cmd(55, rca << 16, R1, NULL) && cmd(idx, arg, flags, resp);
}

static bool set_clock(u32 khz)
{
    u32 c1 = rd(R_CONTROL1);
    c1 &= ~(1u << 2);                                    /* SD clock off while changing */
    wr(R_CONTROL1, c1);
    u32 div = (base_khz + 2 * khz - 1) / (2 * khz);      /* SDHCI 3: f = base / (2 * div) */
    if (div > 0x3FF) div = 0x3FF;
    c1 &= ~0xFFC0u;
    c1 |= ((div & 0xFF) << 8) | ((div >> 8) << 6) | 1;   /* internal clock on */
    c1 = (c1 & ~0xF0000u) | (0xE << 16);                 /* data timeout */
    wr(R_CONTROL1, c1);
    u64 end = timer_ticks() + 100;
    while (!(rd(R_CONTROL1) & 2)) if (timer_ticks() > end) return false;
    wr(R_CONTROL1, rd(R_CONTROL1) | 4);
    timer_busy_wait(2);
    return true;
}

bool sd_init(void)
{
    base = g_periph + (g_pi_model == 4 ? 0x340000 : 0x300000);
    wr(R_CONTROL1, rd(R_CONTROL1) | (1u << 24));         /* reset the host controller */
    u64 end = timer_ticks() + 100;
    while (rd(R_CONTROL1) & (1u << 24)) if (timer_ticks() > end) { kprintf("[sd  ] controller does not reset\n"); return false; }
    wr(R_CONTROL0, 0x0F00);                              /* bus power on, 3.3 V */
    u32 caps = rd(R_CAPS);
    base_khz = ((caps >> 8) & 0xFF) * 1000;
    if (!base_khz) base_khz = g_pi_model == 4 ? 100000 : 41666;
    wr(R_IRPT_MASK, 0xFFFFFFFF);
    wr(R_IRPT_EN, 0);
    if (!set_clock(400)) { kprintf("[sd  ] clock does not start\n"); return false; }
    timer_busy_wait(5);

    u32 r;
    cmd(0, 0, RESP_NONE, NULL);
    bool v2 = cmd(8, 0x1AA, R7, &r) && (r & 0xFFF) == 0x1AA;
    rca = 0;
    for (int i = 0; ; i++) {
        if (!app_cmd(41, 0x00FF8000 | (v2 ? 0x40000000 : 0), R3, &r)) { kprintf("[sd  ] no card answers\n"); return false; }
        if (r & 0x80000000) break;
        if (i > 100) { kprintf("[sd  ] card stays busy\n"); return false; }
        timer_busy_wait(10);
    }
    sdhc = (r & 0x40000000) != 0;
    if (!cmd(2, 0, R2, NULL) || !cmd(3, 0, R6, &r)) { kprintf("[sd  ] card identification failed\n"); return false; }
    rca = r >> 16;
    set_clock(25000);
    if (!cmd(7, rca << 16, R1B, NULL)) { kprintf("[sd  ] cannot select the card\n"); return false; }
    if (!sdhc) cmd(16, 512, R1, NULL);
    if (app_cmd(6, 2, R1, NULL)) wr(R_CONTROL0, rd(R_CONTROL0) | 2);     /* 4-bit bus */
    ok = true;
    kprintf("[sd  ] SD card ready (%s, RCA %04x, base clock %u MHz, host v%u)\n",
            sdhc ? "SDHC/SDXC" : "SDSC", rca, base_khz / 1000, ((rd(R_VERSION) >> 16) & 0xFF) + 1);
    return true;
}

static int xfer(u32 lba, void *buf, bool write)
{
    if (!ok) return -1;
    u32 *w = buf;
    wr(R_BLKSIZECNT, (1u << 16) | 512);
    u32 flags = R1 | DATA | (write ? 0 : READ_DIR);
    if (!cmd(write ? 24 : 17, sdhc ? lba : lba * 512, flags, NULL)) return -1;
    if (!wait_int(write ? INT_WRITE_RDY : INT_READ_RDY, 500)) return -1;
    for (int i = 0; i < 128; i++) {
        if (write) wr(R_DATA, w[i]);
        else w[i] = rd(R_DATA);
    }
    if (!wait_int(INT_DATA_DONE, 1000)) return -1;
    return 0;
}

int sd_read(u32 lba, u32 count, void *buf)
{
    for (u32 i = 0; i < count; i++)
        if (xfer(lba + i, (u8 *)buf + i * 512, false)) return -1;
    return 0;
}

int sd_write(u32 lba, u32 count, const void *buf)
{
    for (u32 i = 0; i < count; i++)
        if (xfer(lba + i, (u8 *)buf + i * 512, true)) return -1;
    return 0;
}
