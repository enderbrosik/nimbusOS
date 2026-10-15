; NimbusOS - kernel entry point. Stage 2 jumps here (0x100000) in 32-bit
; protected mode with paging off, flat segments and EBX = boot info pointer.
[bits 32]
section .text.entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    mov esi, ebx                ; keep boot info pointer
    ; zero .bss (includes our boot stack)
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb
    mov esp, boot_stack_top
    xor ebp, ebp
    push esi
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
global boot_stack_bottom
boot_stack_bottom:
    resb 16384
global boot_stack_top
boot_stack_top:
