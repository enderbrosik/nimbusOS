/* NimbusOS - Raspberry Pi stand-in for arch/x86/io.h.  The Pi build puts
 * arch/arm64/shim first on the include path, so shared code that includes
 * the x86 header gets these AArch64 versions.  There is no port I/O on ARM:
 * those helpers do nothing (the PC-only code that uses them is not built,
 * or touches hardware the Pi does not have). */
#ifndef NIMBUS_IO_H
#define NIMBUS_IO_H
#include "kernel/types.h"

static inline void outb(u16 port, u8 v)  { (void)port; (void)v; }
static inline void outw(u16 port, u16 v) { (void)port; (void)v; }
static inline void outl(u16 port, u32 v) { (void)port; (void)v; }
static inline u8  inb(u16 port) { (void)port; return 0xFF; }
static inline u16 inw(u16 port) { (void)port; return 0xFFFF; }
static inline u32 inl(u16 port) { (void)port; return 0xFFFFFFFF; }
static inline void io_wait(void) {}

static inline void cli(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("wfi"); }

/* DAIF bit 7 (I) set = IRQs masked. */
static inline u32 irq_save(void)
{
    u64 f;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(f) :: "memory");
    return (u32)f;
}
static inline void irq_restore(u32 f) { if (!(f & 0x80)) sti(); }

static inline u64 read_cr0(void) { return 0; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mrs %0, far_el1" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { return 0; }
#endif
