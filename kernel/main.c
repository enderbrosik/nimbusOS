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

    vga_setcolor(VGA_LGREEN, VGA_BLACK);
    kprintf("\nStage 1 OK: NimbusOS booted into 32-bit protected mode.\n");
    for (;;) hlt();
}
