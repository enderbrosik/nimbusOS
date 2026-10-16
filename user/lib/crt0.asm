; NimbusOS user programs - process entry point.
; The kernel starts a program in ring 3 at _start with RDI = argc,
; RSI = argv (strings on the user stack) and RSP pointing just below them.
[bits 64]
section .text
global _start
extern main
extern exit
extern __libc_init

_start:
    and rsp, -16                ; System V ABI: 16-byte aligned before a call
    push rdi
    push rsi
    call __libc_init
    pop rsi
    pop rdi
    call main
    mov edi, eax
    call exit                   ; flushes stdout, never returns
.hang:
    jmp .hang
