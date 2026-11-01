/* NimbusOS - Raspberry Pi stand-in for arch/x86/idt.h: the register frame
 * saved by arch/arm64/vectors.S and the scheduler hooks. */
#ifndef NIMBUS_IDT_H
#define NIMBUS_IDT_H
#include "kernel/types.h"

struct regs {
    u64 x[31];              /* x0..x30 (x29 = frame pointer, x30 = link register) */
    u64 elr, spsr, pad;
};

typedef void (*irq_handler_t)(struct regs *r);
extern uptr (*sched_hook)(uptr sp, int voluntary);
extern uptr (*syscall_hook)(struct regs *r);
extern uptr (*user_fault_hook)(struct regs *r, const char *what);
extern void (*user_return_hook)(struct regs *r);
#endif
