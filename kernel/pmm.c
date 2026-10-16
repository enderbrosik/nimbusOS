/*
 * NimbusOS - physical memory manager: one bit per 4 KiB frame over the full
 * low 4 GiB of physical memory (1 Mi frames -> 128 KiB bitmap in .bss).
 * A set bit means "used / not RAM".  Usable RAM comes from the BIOS E820 map
 * (OSDev wiki "Detecting Memory (x86)", "Page Frame Allocation").
 */
#include "kernel/pmm.h"
#include "kernel/bootinfo.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "arch/x86/io.h"

#define MAX_FRAMES (1024 * 1024)
static u32 bitmap[MAX_FRAMES / 32];
static u32 total_frames, free_frames, search_hint;
static u64 ram_top;
extern char __kernel_start[], __kernel_end[];

static inline void set_used(u32 f) { bitmap[f >> 5] |= 1u << (f & 31); }
static inline void set_free(u32 f) { bitmap[f >> 5] &= ~(1u << (f & 31)); }
static inline bool is_used(u32 f) { return bitmap[f >> 5] & (1u << (f & 31)); }

void pmm_init(void)
{
    memset(bitmap, 0xFF, sizeof bitmap);
    struct e820_entry *e = (struct e820_entry *)(uptr)g_bootinfo.mmap_addr;
    u64 top = 0;
    for (u32 i = 0; i < g_bootinfo.mmap_count; i++) {
        klog("[pmm ] e820 %08x%08x len %08x%08x type %u\n",
             (u32)(e[i].base >> 32), (u32)e[i].base, (u32)(e[i].length >> 32), (u32)e[i].length, e[i].type);
        if (e[i].type != 1) continue;
        u64 start = (e[i].base + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        u64 end = (e[i].base + e[i].length) & ~(u64)(PAGE_SIZE - 1);
        /* Only RAM the VMM identity maps (below 3 GiB) is handed out. */
        if (end > 0xC0000000ULL) end = 0xC0000000ULL;
        for (u64 a = start; a < end; a += PAGE_SIZE) {
            u32 f = (u32)(a >> 12);
            if (is_used(f)) { set_free(f); free_frames++; total_frames++; }
        }
        if (end > top) top = end;
    }
    ram_top = top ? top : 0x2000000;
    if (g_bootinfo.mmap_count == 0) {
        /* No E820: assume 32 MiB of RAM above 1 MiB. */
        for (u32 f = 256; f < 8192; f++) { set_free(f); free_frames++; total_frames++; }
    }
    /* Reserve the first 1 MiB (IVT, BIOS data, boot structures, VGA, ROMs)
     * and the kernel image including .bss. */
    u32 kend = ((uptr)__kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u32 f = 0; f < kend; f++)
        if (!is_used(f)) { set_used(f); free_frames--; }
    search_hint = kend;
    /* Live CD: the RAM disk loaded by stage 2 must never be handed out. */
    if (g_bootinfo.ramdisk_addr) {
        u32 first = g_bootinfo.ramdisk_addr / PAGE_SIZE;
        u32 last = (g_bootinfo.ramdisk_addr + g_bootinfo.ramdisk_sectors * 512 + PAGE_SIZE - 1) / PAGE_SIZE;
        for (u32 f = first; f < last; f++)
            if (!is_used(f)) { set_used(f); free_frames--; }
        kprintf("[pmm ] reserved RAM disk %p, %u KiB\n", (void *)(uptr)g_bootinfo.ramdisk_addr, g_bootinfo.ramdisk_sectors / 2);
    }
    kprintf("[pmm ] %u MiB usable, %u frames free, kernel ends at %p\n",
            total_frames / 256, free_frames, __kernel_end);
}

u32 pmm_alloc(void)
{
    u32 flags = irq_save();
    for (u32 n = 0; n < MAX_FRAMES / 32; n++) {
        u32 w = (search_hint / 32 + n) % (MAX_FRAMES / 32);
        if (bitmap[w] == 0xFFFFFFFF) continue;
        for (u32 b = 0; b < 32; b++) {
            if (!(bitmap[w] & (1u << b))) {
                u32 f = w * 32 + b;
                set_used(f);
                free_frames--;
                search_hint = f;
                irq_restore(flags);
                return f * PAGE_SIZE;
            }
        }
    }
    irq_restore(flags);
    return 0;
}

void pmm_free(u32 phys)
{
    u32 flags = irq_save();
    u32 f = phys / PAGE_SIZE;
    if (is_used(f)) { set_free(f); free_frames++; if (f < search_hint) search_hint = f; }
    irq_restore(flags);
}

u32 pmm_total_frames(void) { return total_frames; }
u64 pmm_ram_top(void) { return ram_top; }
u32 pmm_free_frames(void) { return free_frames; }
