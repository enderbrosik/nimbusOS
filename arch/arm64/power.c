/* NimbusOS - power on the Pi: reboot through the PM watchdog; the Pi has no
 * soft power-off, so "shut down" stops the CPU and says so on screen. */
#include "arch/arm64/pi.h"
#include "kernel/power.h"
#include "kernel/kprintf.h"
#include "arch/x86/io.h"
#include "drivers/fb.h"
#include "gui/gfx.h"

#define PM_RSTC     0x10001C
#define PM_WDOG     0x100024
#define PM_PASSWORD 0x5A000000

void power_init(void) {}
bool power_acpi_available(void) { return false; }

void power_reboot(void)
{
    kprintf("[pwr ] reboot\n");
    sdlog_flush();
    cli();
    mmio_w(g_periph + PM_WDOG, PM_PASSWORD | 10);
    mmio_w(g_periph + PM_RSTC, PM_PASSWORD | (mmio_r(g_periph + PM_RSTC) & ~0x30u) | 0x20);
    for (;;) hlt();
}

void power_shutdown(void)
{
    kprintf("[pwr ] shut down\n");
    sdlog_flush();
    cli();
    if (g_fb.pixels) {
        surface_t s;
        surface_wrap(&s, g_fb.pixels, g_fb.width, g_fb.height, g_fb.pitch);
        gfx_fill_rect(&s, 0, 0, s.w, s.h, 0);
        gfx_text(&s, s.w / 2 - 150, s.h / 2, "It is now safe to unplug your Raspberry Pi.", 0xFFFFFF, COLOR_NONE);
    }
    for (;;) hlt();
}
