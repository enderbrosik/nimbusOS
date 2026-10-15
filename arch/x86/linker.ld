/* NimbusOS kernel linker script: flat binary loaded at 1 MiB by stage2. */
ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    __kernel_start = .;
    .text : {
        *(.text.entry)
        *(.text .text.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) }
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(4096);
    __kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
