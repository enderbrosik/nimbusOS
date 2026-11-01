/* NimbusOS - Raspberry Pi kernel main (stage 1: boots to the desktop on
 * HDMI with an on-screen and SD card log; no keyboard or mouse yet). */
#include "arch/arm64/pi.h"
#include "kernel/version.h"
#include "kernel/types.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "kernel/bootinfo.h"
#include "kernel/panic.h"
#include "arch/x86/io.h"
#include "drivers/vga_text.h"
#include "drivers/pit.h"
#include "drivers/fb.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/task.h"
#include "kernel/proc.h"
#include "kernel/selftest.h"
#include "kernel/power.h"
#include "kernel/settings.h"
#include "kernel/sound.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gui/gfx.h"
#include "fs/vfs.h"
#include "net/net.h"

struct bootinfo g_bootinfo;
struct sysmon_stats g_sysmon;
static struct e820_entry mem_map[1];
extern const unsigned char pi_vga_font[4096];
extern char __disk_start[], __disk_end[];
static volatile u32 mb[16] __attribute__((aligned(16)));

static void gui_task(void *arg)
{
    wm_init();
    kprintf("[gui ] Desktop ready\n");
    sound_event(SND_STARTUP);
    wm_run();
}

static void sysmon_task(void *arg)
{
    for (;;) {
        struct heap_stats hs;
        heap_get_stats(&hs);
        g_sysmon.heap_used = hs.used;
        g_sysmon.heap_mapped = hs.mapped;
        g_sysmon.free_frames = pmm_free_frames();
        g_sysmon.samples++;
        task_sleep(1000);
    }
}

static void gui_panic_screen(const char *text)
{
    surface_t s;
    surface_wrap(&s, g_fb.pixels, g_fb.width, g_fb.height, g_fb.pitch);
    gfx_fill_rect(&s, 0, 0, s.w, s.h, RGB(0, 60, 160));
    int y = 60;
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        gfx_text_n(&s, 60, y, p, n, 0xFFFFFF, COLOR_NONE);
        y += 18;
        p += n + (nl ? 1 : 0);
    }
    gfx_text(&s, 60, y + 18, "The log is in NIMBUS.LOG on the SD card.", 0xFFFFFF, COLOR_NONE);
}

/* Board revision and the ARM's share of RAM, asked before the MMU is on. */
static void query_board(void)
{
    int i = 0;
    mb[i++] = 0; mb[i++] = 0;
    mb[i++] = 0x00010002; mb[i++] = 4; mb[i++] = 0; mb[i++] = 0;            /* board revision */
    mb[i++] = 0x00010005; mb[i++] = 8; mb[i++] = 0; mb[i++] = 0; mb[i++] = 0; /* ARM memory */
    mb[i++] = 0;
    mb[0] = i * 4;
    if (mbox_call(mb)) {
        g_pi_revision = mb[5];
        if (mb[10] >= 0x4000000) g_arm_mem = (u64)mb[9] + mb[10];
    }
}

void pi_main(u64 dtb)
{
    pi_detect();
    pi_led_blink(5);                                /* "the kernel is running", even with no screen */
    pi_led(true);
    uart_init();
    timer_early();
    query_board();
    bool screen = fb_early();                       /* first, so even an early crash is visible */
    vga_init();
    if (screen) kprintf("NimbusOS: kernel started, screen ready, turning on the MMU\n");
    mmu_init();
    kprintf("[boot] MMU on\n");
    sdlog_init();                                   /* early, so a later hang still leaves a log */
    vga_setcolor(VGA_LCYAN, VGA_BLACK);
    kprintf("NimbusOS " NIMBUS_VERSION " - booting on %s\n", pi_board_name());
    vga_setcolor(VGA_LGREY, VGA_BLACK);
    kprintf("[boot] board revision %06x, ARM RAM %u MiB, device tree at %llx, screen %s %dx%d\n",
            g_pi_revision, (u32)(g_arm_mem >> 20), dtb, screen ? "ok" : "MISSING", g_fb.width, g_fb.height);

    mem_map[0].base = 0;
    mem_map[0].length = g_arm_mem;
    mem_map[0].type = 1;
    g_bootinfo.magic = BOOTINFO_MAGIC;
    g_bootinfo.mmap_addr = (u32)(uptr)mem_map;
    g_bootinfo.mmap_count = 1;
    g_bootinfo.font_addr = (u32)(uptr)pi_vga_font;
    g_bootinfo.ramdisk_addr = (u32)(uptr)__disk_start;
    g_bootinfo.ramdisk_sectors = (u32)((__disk_end - __disk_start) / 512);

    pmm_init();
    vmm_init(pmm_ram_top());
    heap_init();
    pit_init();
    tasking_init();
    sti();
    timer_busy_wait(20);
    if (irq_count()) kprintf("[cpu ] timer interrupts working (%llu in 20 ms)\n", irq_count());
    else kprintf("[cpu ] WARNING: no timer interrupts - tasks will only switch when they wait\n");
    if (irq_count()) selftest_core();               /* its scheduler test needs preemption */
    vga_setcolor(VGA_LGREEN, VGA_BLACK);
    kprintf("Core kernel OK.\n");
    vga_setcolor(VGA_LGREY, VGA_BLACK);

    if (!vfs_init()) panic("cannot mount the system volume");
    selftest_fs();
    power_init();
    gfx_init();
    kprintf("Drivers OK. (No keyboard or mouse yet: USB support is the next stage.)\n");
    sdlog_flush();

    settings_load();
    sound_init();
    net_init();
    if (!fb_init(1024, 768)) panic("no framebuffer from the firmware");
    g_screen_w = g_fb.width;
    g_screen_h = g_fb.height;
    theme_set_accent((u32)settings_get_int("accent", ACCENT_DEFAULT));
    panic_display_hook = gui_panic_screen;
    sdlog_start_task();
    task_create_ex("desktop", gui_task, NULL, 256 * 1024);
    task_create("sysmon", sysmon_task, NULL);
    for (;;) task_sleep(1000);
}
