/* NimbusOS user programs: static ELF64 executables at USER_BASE (512 GiB,
 * PML4 entry 1).  Code is built with -mcmodel=large so addresses above
 * 2 GiB work without position-independent code. */
ENTRY(_start)
SECTIONS
{
    . = 0x8000000000;
    .text : { *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : ALIGN(16) { *(COMMON) *(.bss .bss.*) }
    _end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
