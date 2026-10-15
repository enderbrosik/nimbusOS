; NimbusOS - interrupt service routine stubs.
;
; Every vector pushes (error code or 0) and its vector number, then jumps to
; isr_common, which saves the full register state as a `struct regs` (see
; arch/x86/idt.h) and calls isr_dispatch(regs*).  isr_dispatch returns the
; stack pointer to resume from, which lets the scheduler switch tasks simply
; by returning a different task's saved frame.
;
; CPU exceptions that push an error code: 8, 10-14, 17, 21, 29, 30
; (Intel SDM Vol. 3A, Table 6-1 "Protected-Mode Exceptions and Interrupts").
[bits 32]
section .text
extern isr_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 256
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call isr_dispatch
    mov esp, eax                ; possibly a different task's frame
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                  ; vector + error code
    iret

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dd isr %+ i
  %assign i i+1
%endrep

section .text
; void gdt_flush(struct gdt_ptr *p)
global gdt_flush
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload
.reload:
    ret
