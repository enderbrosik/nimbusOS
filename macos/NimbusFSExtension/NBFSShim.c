#include "NBFSShim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

nbfs_t *nbfs_shim_mount(void *ctx, nbfs_shim_io rd, nbfs_shim_wio wr, uint32_t sectors, int *err)
{
    nbfs_t *fs = calloc(1, sizeof *fs);
    if (!fs) { *err = NBFS_ENOSPC; return NULL; }
    struct nbfs_blkdev dev = { .ctx = ctx, .read = rd, .write = wr, .sectors = sectors };
    int r = nbfs_mount(fs, &dev);
    if (r) { free(fs); *err = r; return NULL; }
    *err = 0;
    return fs;
}

void nbfs_shim_close(nbfs_t *fs) { if (fs) { nbfs_sync(fs); free(fs); } }

static int no_write(void *ctx, uint32_t lba, uint32_t count, const void *buf) { return -1; }

int nbfs_shim_probe(void *ctx, nbfs_shim_io rd, uint32_t sectors)
{
    int err = 0;
    nbfs_t *fs = nbfs_shim_mount(ctx, rd, no_write, sectors, &err);   /* a probe never writes */
    if (!fs) return err ? err : NBFS_EBADFS;
    free(fs);
    return 0;
}

static int child_path(nbfs_t *fs, uint32_t dir_ino, const char *name, char *out, int max)
{
    char dir[NBFS_PATH_MAX];
    int r = nbfs_get_path(fs, dir_ino, dir, sizeof dir);
    if (r) return r;
    nbfs_path_join(dir, name, out, max);
    return 0;
}

int nbfs_shim_lookup(nbfs_t *fs, uint32_t dir_ino, const char *name, struct nbfs_stat *st)
{
    char p[NBFS_PATH_MAX];
    int r = child_path(fs, dir_ino, name, p, sizeof p);
    return r ? r : nbfs_stat(fs, p, st);
}

int nbfs_shim_create(nbfs_t *fs, uint32_t dir_ino, const char *name, int type, struct nbfs_stat *st)
{
    char p[NBFS_PATH_MAX];
    int r = child_path(fs, dir_ino, name, p, sizeof p);
    if (r) return r;
    r = type == NBFS_TYPE_DIR ? nbfs_mkdir(fs, p) : nbfs_create(fs, p, type);
    if (r < 0) return r;
    nbfs_sync(fs);
    return nbfs_stat(fs, p, st);
}

int nbfs_shim_remove(nbfs_t *fs, uint32_t dir_ino, const char *name)
{
    char p[NBFS_PATH_MAX];
    int r = child_path(fs, dir_ino, name, p, sizeof p);
    if (r) return r;
    r = nbfs_unlink(fs, p);
    if (r == 0) nbfs_sync(fs);
    return r;
}

int nbfs_shim_rename(nbfs_t *fs, uint32_t from_dir, const char *from, uint32_t to_dir, const char *to)
{
    char a[NBFS_PATH_MAX], b[NBFS_PATH_MAX];
    int r = child_path(fs, from_dir, from, a, sizeof a);
    if (!r) r = child_path(fs, to_dir, to, b, sizeof b);
    if (r) return r;
    r = nbfs_rename(fs, a, b);
    if (r == 0) nbfs_sync(fs);
    return r;
}

const char *nbfs_shim_strerror(int err) { return nbfs_strerror(err); }
