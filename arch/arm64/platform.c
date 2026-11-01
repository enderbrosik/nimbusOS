/* NimbusOS - Raspberry Pi board detection and the VideoCore mailbox.
 * References: Raspberry Pi firmware wiki "Mailbox property interface",
 * BCM2711 ARM Peripherals (ch. 1.2 address map, ch. 6 ARM local). */
#include "arch/arm64/pi.h"
#include "kernel/string.h"

int  g_pi_model = 3;
uptr g_periph = 0x3F000000;
uptr g_local = 0x40000000;
u32  g_pi_revision;
u64  g_arm_mem = 0x3C000000;

#define MBOX_READ   0xB880
#define MBOX_STATUS 0xB898
#define MBOX_WRITE  0xB8A0
#define MBOX_FULL   0x80000000
#define MBOX_EMPTY  0x40000000

void pi_detect(void)
{
    u64 midr = SYSREG(midr_el1);
    if (((midr >> 4) & 0xFFF) == 0xD08) {          /* Cortex-A72: BCM2711 */
        g_pi_model = 4;
        g_periph = 0xFE000000;
        g_local = 0xFF800000;
    }
}

void dcache_clean_inval(const void *p, u64 len)
{
    u64 a = (uptr)p & ~63ull, e = (uptr)p + len;
    for (; a < e; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

bool mbox_call(volatile u32 *buf)
{
    u32 addr = (u32)(uptr)buf;
    dcache_clean_inval((const void *)buf, buf[0]);
    while (mmio_r(g_periph + MBOX_STATUS) & MBOX_FULL) {}
    mmio_w(g_periph + MBOX_WRITE, (addr & ~0xFu) | 8);
    for (u32 tries = 0; tries < 100000000; tries++) {
        while (mmio_r(g_periph + MBOX_STATUS) & MBOX_EMPTY) {}
        u32 r = mmio_r(g_periph + MBOX_READ);
        if ((r & 0xF) == 8) {
            dcache_clean_inval((const void *)buf, buf[0]);
            return buf[1] == 0x80000000;
        }
    }
    return false;
}

const char *pi_board_name(void)
{
    if (g_pi_model == 3) return "Raspberry Pi 3 (BCM2837)";
    if ((g_pi_revision & (1 << 23)) && ((g_pi_revision >> 4) & 0xFF) == 0x13) return "Raspberry Pi 400 (BCM2711)";
    if ((g_pi_revision & (1 << 23)) && ((g_pi_revision >> 4) & 0xFF) == 0x14) return "Raspberry Pi CM4 (BCM2711)";
    return "Raspberry Pi 4 (BCM2711)";
}
