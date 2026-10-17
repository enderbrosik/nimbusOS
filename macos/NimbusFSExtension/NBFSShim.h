/* C side of the NimbusFS FSKit extension: a thin layer that lets Swift use fs/nbfs.c.
 */
#ifndef NBFS_SHIM_H
#define NBFS_SHIM_H
#include <stdint.h>
#include <stdbool.h>
#if __has_include("fs/nbfs.h")
#include "fs/nbfs.h"        /* found through Header Search Paths (a folder that contains fs/nbfs.h) */
#else
#include "nbfs.h"
#endif

typedef int (*nbfs_shim_io)(void *ctx, uint32_t lba, uint32_t count, void *buf);
typedef int (*nbfs_shim_wio)(void *ctx, uint32_t lba, uint32_t count, const void *buf);

/* Mounts NimbusFS on a block device given as two callbacks (512-byte sectors). Returns NULL and sets *err on failure. */
nbfs_t *nbfs_shim_mount(void *ctx, nbfs_shim_io rd, nbfs_shim_wio wr, uint32_t sectors, int *err);
void nbfs_shim_close(nbfs_t *fs);

/* Formats the device as an empty NimbusFS volume. Returns 0 or an NBFS_E* error. */
int nbfs_shim_format(void *ctx, nbfs_shim_io rd, nbfs_shim_wio wr, uint32_t sectors, const char *label, uint32_t now);

/* Reads the superblock only (used by "probe"): returns 0 if this is a NimbusFS volume. */
int nbfs_shim_probe(void *ctx, nbfs_shim_io rd, uint32_t sectors);

/* Inode based helpers (FSKit names items by inode, nbfs by path). */
int nbfs_shim_lookup(nbfs_t *fs, uint32_t dir_ino, const char *name, struct nbfs_stat *st);
int nbfs_shim_create(nbfs_t *fs, uint32_t dir_ino, const char *name, int type, struct nbfs_stat *st);
int nbfs_shim_remove(nbfs_t *fs, uint32_t dir_ino, const char *name);
int nbfs_shim_rename(nbfs_t *fs, uint32_t from_dir, const char *from, uint32_t to_dir, const char *to);
const char *nbfs_shim_strerror(int err);
#endif
