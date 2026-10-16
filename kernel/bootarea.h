/* NimbusOS - the boot area's two kernel slots (see kernel/bootarea.c). */
#ifndef NIMBUS_BOOTAREA_H
#define NIMBUS_BOOTAREA_H
#include "kernel/types.h"

#define KSLOT_A_LBA    64
#define KSLOT_B_LBA    2080
#define KSLOT_SECTORS  2016

struct bootarea_info {
    u32 kernel_lba, kernel_sectors;      /* the slot the MBR boots */
    char active;                         /* 'A' or 'B' */
};
/* 0 on success, else a negative error and *why explains */
int bootarea_info(struct bootarea_info *bi, const char **why);
/* SHA-256 of the first len bytes of the active kernel slot */
int bootarea_active_hash(u32 len, u8 hash[32]);
/* Write a kernel image to the inactive slot, verify it by reading it back,
 * then point the MBR at it.  The old kernel stays in the other slot. */
int bootarea_install_kernel(const u8 *image, u32 len, const char **why);
#endif
