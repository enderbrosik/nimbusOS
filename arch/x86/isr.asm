; NimbusOS - interrupt service routine stubs (64-bit).
;
; Every vector pushes (error code or 0) and its vector number, then jumps to
; isr_common, which saves all general purpose registers as a `struct regs`
; (see arch/x86/idt.h) and calls isr_dispatch(regs*).  isr_dispatch returns
; the stack pointer to resume from, which lets the scheduler switch tasks
; simply by returning a different task's saved frame.
;
; In long mode the CPU always pushes SS:RSP, RFLAGS, CS:RIP (+ error code)
; and aligns RSP to 16 bytes first (Intel SDM Vol. 3A section 6.14.2), so
; the frame below keeps the stack 16-byte aligned for the C call.
;
; CPU exceptions that push an error code: 8, 10-14, 17, 21, 29, 30
; (Intel SDM Vol. 3A, Table 6-1).
[bits 64]
section .text
extern isr_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
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
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    call isr_dispatch
    mov rsp, rax                ; possibly a different task's frame
global isr_return
isr_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                 ; vector + error code
    iretq

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dq isr %+ i
  %assign i i+1
%endrep

section .text
; void gdt_flush(struct gdt_ptr *p) - load the GDT and reload segments.
; CS is reloaded with a far return (there is no far jump to an immediate
; selector in 64-bit mode).
global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    pop rax                     ; return address
    push qword 0x08
    push rax
    retfq

; void tss_flush(u16 selector)
global tss_flush
tss_flush:
    ltr di
    ret
