#ifndef NIMBUS_ATA_H
#define NIMBUS_ATA_H
#include "kernel/types.h"
#define ATA_MAX_DRIVES 4
struct ata_drive {
    bool present;
    int index;              /* 0..3 */
    u16 io, ctrl;           /* port bases */
    bool slave;
    char model[41];
    char name[24];          /* "Primary Master" etc. */
    u32 sectors;            /* LBA28 addressable sectors */
};
extern struct ata_drive g_ata[ATA_MAX_DRIVES];
void ata_init(void);
int  ata_read(int drive, u32 lba, u32 count, void *buf);    /* 0 on success */
int  ata_write(int drive, u32 lba, u32 count, const void *buf);
#endif
