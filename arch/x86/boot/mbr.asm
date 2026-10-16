; =============================================================================
; NimbusOS - Stage 1 bootloader (Master Boot Record)
;
; The BIOS loads this 512-byte sector to 0x0000:0x7C00 and jumps to it with
; DL = BIOS drive number.  We have 416 bytes of code space before the boot
; info block, so the only job here is to load stage 2 from the following
; sectors and jump to it.
;
; Disk reads use INT 13h AH=42h ("Extended Read", LBA addressing) when the
; BIOS supports it, else AH=02h with CHS addresses computed from the AH=08h
; geometry (El Torito hard-disk emulation often lacks the extensions).
; OSDev wiki: "Disk access using the BIOS (INT 13h)".
;
; A small "boot info block" at offset 0x1A0 is patched by tools/mkimage:
;   0x1A0  u32  magic  'NBT1'
;   0x1A4  u32  stage2 LBA
;   0x1A8  u16  stage2 sector count
;   0x1AA  u16  reserved
;   0x1AC  u32  kernel LBA
;   0x1B0  u32  kernel sector count
;   0x1B4  u32  reserved
;   0x1B8  u32  disk signature (standard MBR field)
;   0x1BE  partition table (4 x 16 bytes)
;   0x1FE  0x55 0xAA
; =============================================================================

[bits 16]
[org 0x7C00]

STAGE2_ADDR equ 0x8000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack grows down below us
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_boot
    call print

    ; Disk geometry for the CHS fallback (INT 13h AH=08h).  Needed when the
    ; BIOS lacks the LBA extensions, e.g. El Torito hard-disk emulation.
    mov ah, 0x08
    mov dl, [boot_drive]
    xor di, di
    int 0x13
    jc .no_geom
    and cx, 0x3F
    mov [spt], cx
    movzx dx, dh
    inc dx
    mov [heads], dx
.no_geom:
    xor ax, ax
    mov es, ax

    ; INT 13h extensions present?  (AH=41h, BX=55AAh -> BX=AA55h)
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .load
    cmp bx, 0xAA55
    jne .load
    mov byte [use_ext], 1

.load:
    mov eax, [bi_stage2_lba]
    mov cx, [bi_stage2_count]
    mov bx, STAGE2_ADDR
.next:
    push cx
    push eax
    call read_one
    pop eax
    pop cx
    inc eax
    add bx, 512
    loop .next

    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_ADDR

; read_one: read LBA EAX into 0000:BX (one sector), LBA or CHS addressing
read_one:
    cmp byte [use_ext], 0
    je .chs
    mov [dap_lba], eax
    mov [dap_off], bx
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_err
    ret
.chs:
    xor edx, edx
    movzx ecx, word [spt]
    div ecx                     ; eax = lba / spt, edx = lba % spt
    inc dx
    mov cl, dl                  ; sector (1-based)
    xor edx, edx
    movzx esi, word [heads]
    div esi                     ; eax = cylinder, edx = head
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah                   ; cylinder bits 8-9 in CL[7:6]
    mov dl, [boot_drive]
    mov ax, 0x0201
    int 0x13
    jc disk_err
    ret

disk_err:
    mov si, msg_diskerr
    call print
halt:
    cli
    hlt
    jmp halt

; print: SI -> zero terminated string (BIOS teletype, INT 10h AH=0Eh)
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

boot_drive:  db 0
use_ext:     db 0
spt:         dw 63
heads:       dw 255
msg_boot:    db "NimbusOS MBR", 13, 10, 0
msg_diskerr: db "Disk error", 0

align 4
dap:
    db 0x10                 ; size of packet
    db 0
dap_count:
    dw 1                    ; sectors to read
dap_off:
    dw STAGE2_ADDR          ; offset
    dw 0x0000               ; segment
dap_lba:
    dd 0                    ; LBA low
    dd 0                    ; LBA high

    times 0x1A0 - ($ - $$) db 0
bi_magic:        dd 0x3154424E      ; 'NBT1'
bi_stage2_lba:   dd 1
bi_stage2_count: dw 32
                 dw 0
bi_kernel_lba:   dd 64
bi_kernel_count: dd 0
                 dd 0
disk_signature:  dd 0
                 dw 0
    times 0x1BE - ($ - $$) db 0
partition_table:
    times 64 db 0
    dw 0xAA55
