; =============================================================================
; NimbusOS - Stage 2 bootloader
;
; Loaded by the MBR to 0x0000:0x8000, entered in 16-bit real mode, DL = drive.
;
; Responsibilities (things that need the BIOS, so must happen in real mode):
;   1. Enable the A20 line           (OSDev wiki: "A20 Line")
;   2. Query the memory map (E820)   (OSDev wiki: "Detecting Memory (x86)")
;   3. Copy the 8x16 VGA ROM font    (INT 10h AX=1130h BH=06h, RBIL)
;   4. Load the kernel to 1 MiB      (INT 13h AH=42h into a low bounce
;                                     buffer, copied up using "unreal mode",
;                                     OSDev wiki: "Unreal Mode")
;   5. Switch to 32-bit protected mode and jump to the kernel at 0x100000
;      with EBX = pointer to the boot info structure.
;
; Boot info structure (at 0x5000), mirrored in kernel/bootinfo.h:
;   +0  u32 magic 'NBOI'
;   +4  u32 BIOS boot drive
;   +8  u32 number of E820 entries
;   +12 u32 address of E820 entries (24 bytes each)
;   +16 u32 address of 8x16 font (4096 bytes) or 0
;   +20 u32 kernel LBA
;   +24 u32 kernel sector count
;   +28 u32 disk signature of the boot disk
; =============================================================================

[bits 16]
[org 0x8000]

BOOTINFO     equ 0x5000
MMAP_ADDR    equ 0x5100
MMAP_MAX     equ 64
FONT_ADDR    equ 0x6000
BOUNCE_SEG   equ 0x2000          ; 0x20000 linear
CHUNK        equ 64              ; sectors per BIOS read (32 KiB)
KERNEL_ADDR  equ 0x100000
MBR          equ 0x7C00

stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive], dl

    mov si, msg_hello
    call print

    ; ---- clear boot info -------------------------------------------------
    mov di, BOOTINFO
    mov cx, 16
    xor eax, eax
    rep stosw
    mov dword [BOOTINFO+0], 0x494F424E      ; 'NBOI'
    movzx eax, byte [drive]
    mov [BOOTINFO+4], eax
    mov dword [BOOTINFO+12], MMAP_ADDR
    mov eax, [MBR+0x1AC]
    mov [BOOTINFO+20], eax
    mov eax, [MBR+0x1B0]
    mov [BOOTINFO+24], eax
    mov eax, [MBR+0x1B8]
    mov [BOOTINFO+28], eax

    ; ---- 1. A20 -----------------------------------------------------------
    mov ax, 0x2401              ; BIOS: enable A20
    int 0x15
    in al, 0x92                 ; "fast A20" via System Control Port A
    test al, 2
    jnz .a20_done
    or al, 2
    and al, 0xFE                ; never set bit 0 (that resets the CPU)
    out 0x92, al
.a20_done:

    ; ---- 2. E820 memory map ----------------------------------------------
    mov di, MMAP_ADDR
    xor ebx, ebx
    xor bp, bp
.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150         ; 'SMAP'
    mov ecx, 24
    mov dword [es:di+20], 1     ; ACPI 3.0 "valid" bit
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    jcxz .e820_skip
    inc bp
    add di, 24
.e820_skip:
    test ebx, ebx
    jz .e820_done
    cmp bp, MMAP_MAX
    jb .e820_loop
.e820_done:
    movzx eax, bp
    mov [BOOTINFO+8], eax

    ; ---- 3. VGA ROM font --------------------------------------------------
    push ds
    push es
    push bp
    mov ax, 0x1130
    mov bh, 0x06                ; 8x16 font pointer
    int 0x10                    ; returns ES:BP
    mov ax, es
    test ax, ax
    jz .no_font
    push es
    pop ds
    mov si, bp
    xor ax, ax
    mov es, ax
    mov di, FONT_ADDR
    mov cx, 2048
    rep movsw
    pop bp
    pop es
    pop ds
    mov dword [BOOTINFO+16], FONT_ADDR
    jmp .font_done
.no_font:
    pop bp
    pop es
    pop ds
.font_done:

    ; ---- 4. Load the kernel ----------------------------------------------
    mov si, msg_loading
    call print
    mov eax, [MBR+0x1AC]
    mov [cur_lba], eax
    mov eax, [MBR+0x1B0]
    mov [remaining], eax
    mov dword [dest], KERNEL_ADDR
    test eax, eax
    jz kernel_missing

.load_loop:
    mov eax, [remaining]
    test eax, eax
    jz .load_done
    cmp eax, CHUNK
    jbe .chunk_ok
    mov eax, CHUNK
.chunk_ok:
    mov [chunk], ax
    mov [dap_count], ax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE_SEG
    mov eax, [cur_lba]
    mov [dap_lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc disk_error

    ; Copy the bounce buffer above 1 MiB.  The BIOS may have reloaded
    ; segment registers, so (re-)enter unreal mode before every copy.
    call enter_unreal
    movzx ecx, word [chunk]
    shl ecx, 7                  ; sectors * 512 / 4 = dwords
    mov esi, BOUNCE_SEG * 16
    mov edi, [dest]
    a32 rep movsd

    movzx eax, word [chunk]
    add [cur_lba], eax
    sub [remaining], eax
    shl eax, 9
    add [dest], eax
    mov al, '.'
    call putc
    jmp .load_loop
.load_done:
    mov si, msg_ok
    call print

    ; ---- 5. Protected mode -----------------------------------------------
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry

; Enter "unreal mode": briefly switch to protected mode, load DS/ES with a
; flat 4 GiB descriptor, then return to real mode.  The segment descriptor
; caches keep the 4 GiB limit, allowing 32-bit addressing from real mode.
enter_unreal:
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm               ; flush prefetch queue
.pm:
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    jmp short .rm
.rm:
    pop es
    pop ds
    sti
    ret

kernel_missing:
    mov si, msg_nokernel
    call print
    jmp hang
disk_error:
    mov si, msg_diskerr
    call print
hang:
    cli
    hlt
    jmp hang

print:
    lodsb
    test al, al
    jz .done
    call putc
    jmp print
.done:
    ret

putc:
    pusha
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    popa
    ret

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov ebx, BOOTINFO
    mov eax, KERNEL_ADDR
    jmp eax

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
drive:      db 0
chunk:      dw 0
cur_lba:    dd 0
remaining:  dd 0
dest:       dd 0

align 4
dap:
    db 0x10, 0
dap_count: dw 0
dap_off:   dw 0
dap_seg:   dw 0
dap_lba:   dd 0, 0

; Flat GDT: null, 0x08 = 32-bit code 0..4G, 0x10 = 32-bit data 0..4G
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

msg_hello:    db "NimbusOS stage2", 13, 10, 0
msg_loading:  db "Loading kernel", 0
msg_ok:       db " ok", 13, 10, 0
msg_nokernel: db "No kernel!", 0
msg_diskerr:  db "Disk read error", 0
