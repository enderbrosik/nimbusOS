#ifndef NIMBUS_IDT_H
#define NIMBUS_IDT_H
#include "kernel/types.h"

/* Register frame pushed by isr_common (arch/x86/isr.asm). */
struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;                        /* pushed by CPU */
};

#define IRQ_BASE 32
#define SYSCALL_YIELD 0x30
#define SYSCALL_VECTOR 0x80   /* user mode system calls (DPL 3 gate) */

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void irq_register(int irq, irq_handler_t h);
void pic_unmask(int irq);
void pic_mask(int irq);
/* The scheduler hooks in here: called after each IRQ0 / yield, may return
 * a different stack pointer to switch tasks. */
extern uptr (*sched_hook)(uptr sp, int voluntary);
/* System call handler (int 0x80) and user-mode fault handler; both may
 * return a different stack pointer like sched_hook. */
extern uptr (*syscall_hook)(struct regs *r);
extern uptr (*user_fault_hook)(struct regs *r, const char *what);
#endif
