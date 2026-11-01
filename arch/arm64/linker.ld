/* NimbusOS - Raspberry Pi (AArch64) kernel layout.  The firmware loads
 * kernel8.img at 0x80000 (config.txt sets kernel_address to be sure). */
ENTRY(_start)
SECTIONS
{
    . = 0x80000;
    __kernel_start = .;
    .text : { KEEP(*(.text.boot)) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    .disk : { __disk_start = .; KEEP(*(.disk)) __disk_end = .; }
    . = ALIGN(4096);
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        . += 0x10000;
        __stack_top = .;
        . = ALIGN(4096);
        __bss_end = .;
    }
    __kernel_end = .;
    __image_size = __kernel_end - __kernel_start;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
