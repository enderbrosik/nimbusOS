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
#include "kernel/drvdemo.h"
#include "drivers/ps2.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/fb.h"
#include "drivers/ata.h"
#include "gui/gfx.h"

struct bootinfo g_bootinfo;

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
    kprintf("[cpu ] GDT loaded\n");
    idt_init();
    kprintf("[cpu ] IDT loaded, PIC remapped to 0x20-0x2F\n");
    sti();
    __asm__ volatile("int3");
    kprintf("[cpu ] breakpoint exception handled and returned\n");

    /* ---- Stage 2: core kernel ---- */
    pmm_init();
    vmm_init();
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
    gfx_init();
    if (!fb_init(1024, 768)) panic("no linear framebuffer (run QEMU with -vga std)");
    mouse_set_bounds(g_fb.width, g_fb.height);
    kprintf("Drivers OK.\n");
    drvdemo_run();
}
