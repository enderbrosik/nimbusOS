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

/* Seconds-based deadline from the generic timer (works before timer_early). */
static u64 deadline(u32 ms) { return SYSREG(cntpct_el0) + SYSREG(cntfrq_el0) / 1000 * ms; }
static bool past(u64 d) { return SYSREG(cntpct_el0) > d; }

bool mbox_call(volatile u32 *buf)
{
    u32 addr = (u32)(uptr)buf;
    dcache_clean_inval((const void *)buf, buf[0]);
    u64 d = deadline(500);
    while (mmio_r(g_periph + MBOX_STATUS) & MBOX_FULL) if (past(d)) return false;
    mmio_w(g_periph + MBOX_WRITE, (addr & ~0xFu) | 8);
    d = deadline(1000);
    for (;;) {
        while (mmio_r(g_periph + MBOX_STATUS) & MBOX_EMPTY) if (past(d)) return false;
        u32 r = mmio_r(g_periph + MBOX_READ);
        if ((r & 0xF) == 8) {
            dcache_clean_inval((const void *)buf, buf[0]);
            return buf[1] == 0x80000000;
        }
    }
}

/* The green activity LED (GPIO 42 on the Pi 4 and Pi 400; the Pi 3's is
 * behind the firmware, so nothing happens there). */
void pi_led(bool on)
{
    if (g_pi_model != 4) return;
    u32 sel = mmio_r(g_periph + 0x200010);
    sel = (sel & ~(7u << 6)) | (1u << 6);                  /* GPFSEL4: GPIO 42 output */
    mmio_w(g_periph + 0x200010, sel);
    mmio_w(g_periph + (on ? 0x200020 : 0x20002C), 1u << 10); /* GPSET1 / GPCLR1 */
}

void pi_led_blink(int times)
{
    for (int i = 0; i < times; i++) {
        pi_led(true);
        u64 d = deadline(120); while (!past(d)) {}
        pi_led(false);
        d = deadline(120); while (!past(d)) {}
    }
}

const char *pi_board_name(void)
{
    if (g_pi_model == 3) return "Raspberry Pi 3 (BCM2837)";
    if ((g_pi_revision & (1 << 23)) && ((g_pi_revision >> 4) & 0xFF) == 0x13) return "Raspberry Pi 400 (BCM2711)";
    if ((g_pi_revision & (1 << 23)) && ((g_pi_revision >> 4) & 0xFF) == 0x14) return "Raspberry Pi CM4 (BCM2711)";
    return "Raspberry Pi 4 (BCM2711)";
}
