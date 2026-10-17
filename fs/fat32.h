/* NimbusOS - FAT32 volumes (USB-stick style disks), read and write.
 * One FAT32 volume can be mounted at a time, from an ATA drive other than the boot disk.
 * Paths use '/' and are case insensitive; long file names are supported. */
#ifndef NIMBUS_FAT32_H
#define NIMBUS_FAT32_H
#include "kernel/types.h"

#define FAT_NAME_MAX 255

struct fat_item {
    char name[FAT_NAME_MAX + 1];
    bool dir;
    u32 size;
};

/* Mounts the first FAT32 volume found on an ATA drive that is not the boot disk
 * (a partition of type 0B/0C, or a disk that is one FAT32 volume).  Returns 0 or a negative error. */
int  fat32_mount(void);
bool fat32_mounted(void);
const char *fat32_label(void);
void fat32_info(int *drive, u32 *total_kib, u32 *free_kib);
int  fat32_list(const char *path, struct fat_item **out);          /* *out is kmalloc'ed; returns count or an error */
int  fat32_read(const char *path, char **out, u32 *len);           /* kmalloc'ed, NUL terminated */
int  fat32_write(const char *path, const void *buf, u32 len);      /* creates or replaces a file */
int  fat32_mkdir(const char *path);
int  fat32_remove(const char *path);                               /* a file or an empty folder */
#endif
