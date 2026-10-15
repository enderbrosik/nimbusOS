/*
 * NimbusOS - Global Descriptor Table.  We use a flat model: one ring-0 code
 * and one ring-0 data segment covering all 4 GiB.  Descriptor format:
 * Intel SDM Vol. 3A section 3.4.5 "Segment Descriptors"; OSDev wiki "GDT".
 */
#include "arch/x86/gdt.h"
#include "kernel/types.h"

struct gdt_entry {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  gran;       /* flags (high nibble) | limit 19:16 (low nibble) */
    u8  base_hi;
} PACKED;

struct gdt_ptr { u16 limit; u32 base; } PACKED;

static struct gdt_entry gdt[3];
static struct gdt_ptr gp;
extern void gdt_flush(struct gdt_ptr *p);

static void set_entry(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].base_lo  = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].access   = access;
    gdt[i].gran     = (flags << 4) | ((limit >> 16) & 0x0F);
    gdt[i].base_hi  = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xC); /* present, ring0, code, exec/read; 4K gran, 32-bit */
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC); /* present, ring0, data, read/write */
    gp.limit = sizeof(gdt) - 1;
    gp.base = (u32)&gdt;
    gdt_flush(&gp);
}
