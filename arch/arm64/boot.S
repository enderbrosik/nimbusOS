/* NimbusOS - Raspberry Pi entry point (AArch64).
 * The firmware starts every core here (at EL2, or EL3 under some loaders).
 * Core 0 drops to EL1, sets up a stack, clears .bss and calls pi_main;
 * the others sleep forever. */
.section .text.boot
.global _start
_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, 2f
1:  wfe
    b       1b
2:  mov     x19, x0                     /* device tree address from the firmware (unused for now) */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #3
    b.ne    3f
    /* EL3 -> EL2: non-secure, EL2 is AArch64 */
    mov     x1, #0x5b1
    msr     scr_el3, x1
    mov     x1, #0x3c9
    msr     spsr_el3, x1
    adr     x1, 3f
    msr     elr_el3, x1
    eret
3:  mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    4f
    /* EL2 -> EL1: let EL1 use the physical timer, EL1 is AArch64 */
    mov     x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1
    mov     x1, #0x33ff
    msr     cptr_el2, x1                /* no FP/SIMD traps to EL2 */
    msr     hstr_el2, xzr
    ldr     x1, =0x30d00800
    msr     sctlr_el1, x1
    mov     x1, #0x3c5                  /* EL1h, interrupts masked */
    msr     spsr_el2, x1
    adr     x1, 4f
    msr     elr_el2, x1
    eret
4:  ldr     x1, =__stack_top
    mov     sp, x1
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
5:  cmp     x1, x2
    b.hs    6f
    str     xzr, [x1], #8
    b       5b
6:  ldr     x1, =vectors
    msr     vbar_el1, x1
    isb
    mov     x0, x19
    bl      pi_main
    b       1b

/* The disk image (NimbusFS volume) is linked into the kernel; the RAM disk
 * code mounts it exactly like the PC live CD. */
.section .disk, "a"
.incbin DISK_IMAGE
