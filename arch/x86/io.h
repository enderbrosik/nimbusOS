/* NimbusOS - x86 port I/O and CPU helpers. */
#ifndef NIMBUS_IO_H
#define NIMBUS_IO_H
#include "kernel/types.h"

static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline void insw(u16 port, void *buf, size_t count)
{
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void outsw(u16 port, const void *buf, size_t count)
{
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port));
}

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

/* Save RFLAGS and disable interrupts; restore with irq_restore().  Only
 * the IF bit (bit 9) matters, so the low 32 bits are enough. */
static inline u32 irq_save(void)
{
    u64 f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return (u32)f;
}
static inline void irq_restore(u32 f)
{
    if (f & 0x200) sti();
}

static inline u64 read_cr0(void) { u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

#endif
