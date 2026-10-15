#ifndef NIMBUS_IDT_H
#define NIMBUS_IDT_H
#include "kernel/types.h"

/* Register frame pushed by isr_common (arch/x86/isr.asm). */
struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;   /* pusha */
    u32 int_no, err_code;
    u32 eip, cs, eflags;                                 /* pushed by CPU */
};

#define IRQ_BASE 32
#define SYSCALL_YIELD 0x30

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void irq_register(int irq, irq_handler_t h);
void pic_unmask(int irq);
void pic_mask(int irq);
/* The scheduler hooks in here: called after each IRQ0 / yield, may return
 * a different stack pointer to switch tasks. */
extern u32 (*sched_hook)(u32 esp, int voluntary);
#endif
