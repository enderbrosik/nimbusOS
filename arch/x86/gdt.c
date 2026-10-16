/*
 * NimbusOS - Global Descriptor Table and Task State Segment (long mode).
 *
 * In 64-bit mode segmentation is mostly disabled: base and limit are ignored
 * for CS/DS/ES/SS, and only the L (64-bit code), DPL and present bits of the
 * code descriptors matter.  The TSS no longer switches tasks; it only holds
 * RSP0, the kernel stack loaded when an interrupt arrives in ring 3.
 * Intel SDM Vol. 3A sections 3.4.5 "Segment Descriptors", 5.2.1 "Code-
 * Segment Descriptor in 64-bit Mode" and 8.7 "Task Management in 64-bit Mode".
 */
#include "arch/x86/gdt.h"
#include "kernel/string.h"

struct gdt_ptr { u16 limit; u64 base; } PACKED;

struct tss64 {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} PACKED;

static u64 gdt[7];              /* null, kcode, kdata, udata, ucode, tss (2 slots) */
static struct gdt_ptr gp;
static struct tss64 tss;
extern void gdt_flush(struct gdt_ptr *p);
extern void tss_flush(u16 sel);

static u64 seg(u8 access, u8 flags)
{
    /* base 0, limit 0xFFFFF (ignored in long mode except for data segments) */
    return 0xFFFFull | (0xFull << 48) | ((u64)access << 40) | ((u64)flags << 52);
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = seg(0x9A, 0xA);    /* present, ring 0, code exec/read; G=1, L=1 */
    gdt[2] = seg(0x92, 0xC);    /* present, ring 0, data read/write */
    gdt[3] = seg(0xF2, 0xC);    /* present, ring 3, data read/write */
    gdt[4] = seg(0xFA, 0xA);    /* present, ring 3, 64-bit code */
    /* 16-byte system descriptor for the TSS (type 9 = available 64-bit TSS) */
    memset(&tss, 0, sizeof tss);
    tss.iomap_base = sizeof tss;            /* no I/O permission bitmap */
    u64 base = (u64)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
    gp.limit = sizeof(gdt) - 1;
    gp.base = (u64)&gdt;
    gdt_flush(&gp);
    tss_flush(TSS_SEL);
}

void tss_set_kernel_stack(uptr rsp0) { tss.rsp0 = rsp0; }
