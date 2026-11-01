/* NimbusOS - AArch64 exception vectors.  Every exception saves a struct regs
 * (arch/arm64/shim/arch/x86/idt.h) on the current stack and calls
 * exc_dispatch(regs, kind), which returns the stack pointer to resume from:
 * the same frame, or another task's (that is the context switch). */
.macro ENTRY kind
    .balign 0x80
    sub     sp, sp, #272
    stp     x0, x1, [sp, #0]
    mov     x0, #\kind
    b       exc_common
.endm

.section .text
.balign 0x800
.global vectors
vectors:
    ENTRY 4                 /* current EL with SP_EL0: unexpected */
    ENTRY 4
    ENTRY 4
    ENTRY 4
    ENTRY 0                 /* current EL with SP_ELx: sync */
    ENTRY 1                 /* IRQ */
    ENTRY 2                 /* FIQ */
    ENTRY 3                 /* SError */
    ENTRY 4                 /* lower EL (AArch64): not used yet */
    ENTRY 4
    ENTRY 4
    ENTRY 4
    ENTRY 4                 /* lower EL (AArch32) */
    ENTRY 4
    ENTRY 4
    ENTRY 4

exc_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x1, elr_el1
    stp     x30, x1, [sp, #240]
    mrs     x1, spsr_el1
    str     x1, [sp, #256]
    mov     x1, x0
    mov     x0, sp
    bl      exc_dispatch
    mov     sp, x0
    ldr     x1, [sp, #256]
    msr     spsr_el1, x1
    ldp     x30, x1, [sp, #240]
    msr     elr_el1, x1
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #272
    eret
