/* NimbusOS - kernel main. */
#include "kernel/types.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "kernel/bootinfo.h"
#include "kernel/panic.h"
#include "arch/x86/io.h"
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "drivers/serial.h"
#include "drivers/vga_text.h"
#include "drivers/pit.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/task.h"
#include "kernel/selftest.h"
#include "kernel/power.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "kernel/settings.h"
#include "kernel/sound.h"
#include "net/net.h"
#include "drivers/ps2.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/fb.h"
#include "drivers/ata.h"
#include "gui/gfx.h"
#include "fs/vfs.h"

struct bootinfo g_bootinfo;

/* The whole GUI (window manager, desktop and applications) runs in this task. */
static void gui_task(void *arg)
{
    wm_init();
    kprintf("[gui ] Desktop ready\n");
    sound_event(SND_STARTUP);
    wm_run();
}

/* Background task: samples memory statistics once a second (shown by
 * Task Manager) and demonstrates a second long-running kernel thread. */
struct sysmon_stats g_sysmon;
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

/* Blue screen for panics once the framebuffer is active. */
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
}

void kmain(struct bootinfo *bi)
{
    serial_init();
    vga_init();
    vga_setcolor(VGA_LCYAN, VGA_BLACK);
    kprintf("NimbusOS 0.1 - booting\n");
    vga_setcolor(VGA_LGREY, VGA_BLACK);

    if (!bi || bi->magic != BOOTINFO_MAGIC) panic("bad boot info (magic %08x)", bi ? bi->magic : 0);
    g_bootinfo = *bi;
    kprintf("[boot] drive %02x, kernel %u sectors at LBA %u, disk sig %08x\n",
            bi->boot_drive, bi->kernel_sectors, bi->kernel_lba, bi->disk_signature);

    gdt_init();
    kprintf("[cpu ] long mode active, GDT + TSS loaded\n");
    idt_init();
    kprintf("[cpu ] IDT loaded, PIC remapped to 0x20-0x2F\n");
    sti();
    __asm__ volatile("int3");
    kprintf("[cpu ] breakpoint exception handled and returned\n");

    /* ---- Stage 2: core kernel ---- */
    pmm_init();
    vmm_init(pmm_ram_top());
    heap_init();
    pit_init();
    tasking_init();
    selftest_core();
    vga_setcolor(VGA_LGREEN, VGA_BLACK);
    kprintf("Core kernel OK.\n");
    vga_setcolor(VGA_LGREY, VGA_BLACK);

#ifdef PANIC_TEST
    kprintf("PANIC_TEST: dereferencing a bad pointer...\n");
    *(volatile u32 *)0xBADBAD00 = 1;
#endif

    /* ---- Stage 3: drivers ---- */
    ps2_controller_init();
    keyboard_init();
    mouse_init();
    ata_init();
    pci_enumerate();

    /* ---- Stage 4: file system ---- */
    if (!vfs_init()) panic("cannot mount the system volume");
    selftest_fs();
    power_init();
    gfx_init();
    kprintf("Drivers OK.\n");

    /* ---- Stage 5: desktop ---- */
    settings_load();
    sound_init();
    net_init();
    int rw = 1024, rh = 768;
    const char *res = settings_get("resolution", "1024x768");
    char *x;
    rw = (int)strtol(res, &x, 10);
    if (*x == 'x') rh = (int)strtol(x + 1, NULL, 10);
    if (!fb_init(rw, rh)) panic("no linear framebuffer (run QEMU with -vga std)");
    g_screen_w = g_fb.width;
    g_screen_h = g_fb.height;
    theme_set_accent((u32)settings_get_int("accent", ACCENT_DEFAULT));
    panic_display_hook = gui_panic_screen;
    task_create_ex("desktop", gui_task, NULL, 256 * 1024);
    task_create("sysmon", sysmon_task, NULL);
    for (;;) task_sleep(1000);
}
