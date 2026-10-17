/*
 * NimbusFS (NBFS) - a small ext2-inspired file system.
 *
 * This file and fs/nbfs.c are compiled both into the kernel and into the
 * host tools (tools/nfstool.c, tools/fstest.c), so they only depend on a
 * handful of string functions and on a caller-provided block device.
 *
 * On-disk layout (block size 1024 bytes = 2 sectors):
 *   block 0                 reserved (boot code / unused)
 *   block 1                 superblock
 *   block_bitmap_start ...  1 bit per block (1 = used)
 *   inode_bitmap_start ...  1 bit per inode (1 = used); inode 0 is never used
 *   inode_table_start ...   128-byte inodes, 8 per block
 *   data_start ...          file and directory data
 *
 * Inodes address data through 24 direct pointers, one single-indirect block
 * (256 pointers) and one double-indirect block (256 x 256 pointers): max file
 * size = (24 + 256 + 65536) KiB ~= 64 MiB.
 *
 * A directory is a file of 64-byte entries {inode, type, name_len, name[58]};
 * inode == 0 marks a free slot.  "." and ".." are not stored; every inode
 * records its parent directory instead.  Inode 1 is the root directory.
 * All integers are little-endian.
 */
#ifndef NIMBUS_NBFS_H
#define NIMBUS_NBFS_H

#ifdef NIMBUS_KERNEL
#include "kernel/types.h"
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#endif

#define NBFS_MAGIC        0x5346424Eu    /* "NBFS" */
#define NBFS_VERSION      1
#define NBFS_BLOCK        1024
#define NBFS_SECT_PER_BLK 2
#define NBFS_INODE_SIZE   128
#define NBFS_INODES_PER_BLK (NBFS_BLOCK / NBFS_INODE_SIZE)
#define NBFS_NDIRECT      24
#define NBFS_PTRS         (NBFS_BLOCK / 4)
#define NBFS_DIRENT_SIZE  64
#define NBFS_NAME_MAX     57
#define NBFS_ROOT_INO     1
#define NBFS_PATH_MAX     256
#define NBFS_MAX_FILE_BLOCKS (NBFS_NDIRECT + NBFS_PTRS + NBFS_PTRS * NBFS_PTRS)

#define NBFS_TYPE_FREE 0
#define NBFS_TYPE_FILE 1
#define NBFS_TYPE_DIR  2

/* error codes (negative return values) */
#define NBFS_OK            0
#define NBFS_ENOENT       -2
#define NBFS_EIO          -5
#define NBFS_EEXIST      -17
#define NBFS_ENOTDIR     -20
#define NBFS_EISDIR      -21
#define NBFS_EINVAL      -22
#define NBFS_ENOSPC      -28
#define NBFS_ENAMETOOLONG -36
#define NBFS_ENOTEMPTY   -39
#define NBFS_EBADFS      -77

struct nbfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t block_bitmap_start, block_bitmap_blocks;
    uint32_t inode_bitmap_start, inode_bitmap_blocks;
    uint32_t inode_table_start, inode_table_blocks;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t root_inode;
    uint32_t created;
    char     label[32];
} __attribute__((packed));

struct nbfs_inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t ctime;             /* creation time, seconds since 1970 */
    uint32_t mtime;             /* last modification */
    uint32_t parent;            /* parent directory inode */
    uint32_t direct[NBFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
    uint32_t reserved;
} __attribute__((packed));

struct nbfs_dirent {
    uint32_t inode;
    uint8_t  type;
    uint8_t  name_len;
    char     name[58];
} __attribute__((packed));

/* Block device supplied by the caller; works in 512-byte sectors. */
struct nbfs_blkdev {
    void *ctx;
    int (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    uint32_t sectors;
};

#define NBFS_CACHE_SIZE 64

struct nbfs_cache_entry {
    uint32_t block;
    bool     valid;
    uint8_t  data[NBFS_BLOCK];
};

typedef struct nbfs {
    struct nbfs_blkdev dev;
    struct nbfs_super sb;
    bool sb_dirty;
    uint32_t (*now)(void);      /* time source for timestamps (may be NULL) */
    uint32_t block_hint, inode_hint;
    struct nbfs_cache_entry cache[NBFS_CACHE_SIZE];   /* write-through cache */
    uint8_t scratch[NBFS_BLOCK];
} nbfs_t;

struct nbfs_stat {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
    uint32_t ctime, mtime;
    uint32_t parent;
};

struct nbfs_entry {
    uint32_t inode;
    uint32_t type;
    char name[NBFS_NAME_MAX + 1];
};

/* volume */
int  nbfs_format(struct nbfs_blkdev *dev, const char *label, uint32_t now);
int  nbfs_mount(nbfs_t *fs, struct nbfs_blkdev *dev);
int  nbfs_sync(nbfs_t *fs);
void nbfs_statfs(nbfs_t *fs, uint32_t *total_bytes, uint32_t *free_bytes);

/* paths (absolute, '/' separated; "." and ".." are understood) */
int nbfs_lookup(nbfs_t *fs, const char *path, uint32_t *ino);
int nbfs_stat(nbfs_t *fs, const char *path, struct nbfs_stat *st);
int nbfs_stat_inode(nbfs_t *fs, uint32_t ino, struct nbfs_stat *st);
int nbfs_create(nbfs_t *fs, const char *path, int type);     /* returns inode or error */
int nbfs_mkdir(nbfs_t *fs, const char *path);
int nbfs_unlink(nbfs_t *fs, const char *path);               /* file or empty directory */
int nbfs_remove_tree(nbfs_t *fs, const char *path);          /* recursive delete */
int nbfs_rename(nbfs_t *fs, const char *from, const char *to);
int nbfs_get_path(nbfs_t *fs, uint32_t ino, char *out, int max);

/* data */
int nbfs_read(nbfs_t *fs, uint32_t ino, uint32_t off, void *buf, uint32_t len);
int nbfs_write(nbfs_t *fs, uint32_t ino, uint32_t off, const void *buf, uint32_t len);
int nbfs_truncate(nbfs_t *fs, uint32_t ino, uint32_t size);

/* directories: *cookie starts at 0; returns 1 = entry, 0 = end, <0 error */
int nbfs_readdir(nbfs_t *fs, uint32_t dir_ino, uint32_t *cookie, struct nbfs_entry *out);

/* convenience: whole-file helpers */
int nbfs_read_file(nbfs_t *fs, const char *path, void *buf, uint32_t max);
int nbfs_write_file(nbfs_t *fs, const char *path, const void *buf, uint32_t len);

const char *nbfs_strerror(int err);

/* Consistency check: verifies bitmaps, counts and the directory tree.
 * Returns 0 if clean, otherwise the number of problems (details through
 * the optional report callback). */
int nbfs_check(nbfs_t *fs, void (*report)(const char *msg));

/* Path helpers (pure string functions) */
void nbfs_path_normalize(const char *in, char *out, int max);  /* resolves . and .. textually */
void nbfs_path_join(const char *dir, const char *name, char *out, int max);
void nbfs_path_parent(const char *path, char *out, int max);
const char *nbfs_path_basename(const char *path);

#endif
