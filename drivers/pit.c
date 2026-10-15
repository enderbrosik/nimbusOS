/*
 * NimbusOS - 8253/8254 Programmable Interval Timer, channel 0 -> IRQ0.
 * Mode 2 (rate generator), divisor = 1193182 / TIMER_HZ.
 * Intel 8254 datasheet; OSDev wiki "Programmable Interval Timer".
 */
#include "drivers/pit.h"
#include "arch/x86/io.h"
#include "arch/x86/idt.h"
#include "kernel/kprintf.h"

#define PIT_FREQ 1193182
static volatile u64 ticks;

static void pit_irq(struct regs *r) { (void)r; ticks++; }

void pit_init(void)
{
    u32 div = PIT_FREQ / TIMER_HZ;
    outb(0x43, 0x34);           /* channel 0, lobyte/hibyte, mode 2, binary */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_register(0, pit_irq);
    kprintf("[pit ] timer at %u Hz\n", TIMER_HZ);
}

u64 timer_ticks(void)
{
    u32 f = irq_save();
    u64 t = ticks;
    irq_restore(f);
    return t;
}

void timer_busy_wait(u32 ms)
{
    u64 end = timer_ticks() + ms;
    while (timer_ticks() < end) hlt();
}
