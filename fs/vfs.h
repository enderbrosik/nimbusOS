#ifndef NIMBUS_VFS_H
#define NIMBUS_VFS_H
#include "kernel/types.h"
#include "fs/nbfs.h"

#define NBFS_PART_TYPE 0x4E

/* A mounted NimbusFS volume on an ATA drive partition. */
struct volume {
    nbfs_t fs;
    int drive;
    u32 part_lba, part_sectors;
    struct nbfs_blkdev dev;
};

extern struct volume *g_root;      /* the system volume (boot disk) */
#define ROOTFS (&g_root->fs)

int  boot_drive_index(void);       /* ATA index of the disk we booted from, -1 if unknown */
bool find_nbfs_partition(int drive, u32 *lba, u32 *sectors);
struct volume *volume_mount(int drive);
void volume_unmount(struct volume *v);
void blkdev_for_partition(struct nbfs_blkdev *dev, int drive, u32 lba, u32 sectors);
bool vfs_init(void);

/* convenience wrappers on the root volume */
int  vfs_read_all(const char *path, char **out, u32 *len);   /* kmalloc'ed, NUL terminated */
int  vfs_write_all(const char *path, const void *buf, u32 len);
bool vfs_exists(const char *path);
bool vfs_is_dir(const char *path);
/* creates "base", or "base (2)", "base (3)"... in dir; writes the path chosen */
int  vfs_create_unique(const char *dir, const char *base, const char *ext, int type, char *out, int max);
struct vfs_item {
    char name[NBFS_NAME_MAX + 1];
    u32 type, size, mtime, ctime;
};
/* Lists a directory, folders first then files, alphabetically (case
 * insensitive).  *out is kmalloc'ed; returns count or a negative error. */
int  vfs_list(const char *path, struct vfs_item **out);
void vfs_format_size(u32 bytes, char *out, int max);
void vfs_format_time(u32 t, char *out, int max);
#endif
