; NimbusOS - kernel entry point. Stage 2 jumps here (0x100000) in 32-bit
; protected mode with paging off, flat segments and EBX = boot info pointer.
;
; This stub switches the CPU to 64-bit long mode (Intel SDM Vol. 3A section
; 9.8.5 "Initializing IA-32e Mode"; AMD APM Vol. 2 section 14.6):
;   1. check CPUID for long mode support,
;   2. build temporary 4-level page tables identity mapping the first 1 GiB
;      with 2 MiB pages (kernel/vmm.c replaces them with the real ones),
;   3. CR4.PAE = 1, CR3 = PML4, EFER.LME = 1, CR0.PG = 1,
;   4. load a GDT with a 64-bit code segment and far jump into it.
[bits 32]
section .text.entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    mov esi, ebx                ; keep boot info pointer
    ; zero .bss (includes our boot stack and the boot page tables)
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    ; ---- 1. long mode available? (CPUID 0x80000001, EDX bit 29)
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; ---- 2. boot page tables: PML4[0] -> PDPT, PDPT[0] -> PD, PD = 512 x 2 MiB
    mov eax, boot_pdpt
    or eax, 3                   ; present | writable
    mov [boot_pml4], eax
    mov eax, boot_pd
    or eax, 3
    mov [boot_pdpt], eax
    xor ecx, ecx
.fill_pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                ; present | writable | page size (2 MiB)
    mov [boot_pd + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jb .fill_pd

    ; ---- 3. enable PAE, load CR3, set EFER.LME, enable paging
    mov eax, cr4
    or eax, 1 << 5              ; PAE
    mov cr4, eax
    mov eax, boot_pml4
    mov cr3, eax
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, 1 << 8              ; LME
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)   ; PG | WP
    mov cr0, eax

    ; ---- 4. 64-bit GDT and far jump into long mode
    lgdt [boot_gdt_ptr]
    jmp 0x08:long_entry

.no_long_mode:
    ; Print a message on the VGA text screen and stop.
    mov edi, 0xB8000
    mov esi, no_lm_msg
.msg:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x4F                ; white on red
    stosw
    jmp .msg
.hang:
    cli
    hlt
    jmp .hang

[bits 64]
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, boot_stack_top
    xor rbp, rbp
    mov edi, esi                ; kmain(struct bootinfo *) - zero-extended
    call kmain
.hang64:
    cli
    hlt
    jmp .hang64

section .rodata
no_lm_msg: db "NimbusOS needs a 64-bit (x86-64) CPU. Halted.", 0

align 16
boot_gdt:
    dq 0                        ; null
    dq 0x00AF9A000000FFFF       ; 0x08: 64-bit code, ring 0 (L=1)
    dq 0x00CF92000000FFFF       ; 0x10: data, ring 0
boot_gdt_end:
boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dd boot_gdt

section .bss
align 4096
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_pd:   resb 4096
align 16
global boot_stack_bottom
boot_stack_bottom:
    resb 16384
global boot_stack_top
boot_stack_top:
