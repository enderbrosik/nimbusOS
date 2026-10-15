/* Boot information passed by stage2 (see arch/x86/boot/stage2.asm). */
#ifndef NIMBUS_BOOTINFO_H
#define NIMBUS_BOOTINFO_H
#include "kernel/types.h"

#define BOOTINFO_MAGIC 0x494F424E  /* 'NBOI' */

struct e820_entry {
    u64 base;
    u64 length;
    u32 type;       /* 1 = usable RAM */
    u32 acpi;
} PACKED;

struct bootinfo {
    u32 magic;
    u32 boot_drive;
    u32 mmap_count;
    u32 mmap_addr;
    u32 font_addr;
    u32 kernel_lba;
    u32 kernel_sectors;
    u32 disk_signature;
} PACKED;

extern struct bootinfo g_bootinfo;
#endif
