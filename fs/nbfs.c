/*
 * NimbusFS implementation.  See fs/nbfs.h for the on-disk format.
 *
 * Shared between the kernel and the host tools: it only needs memcpy /
 * memset / memcmp / strlen / strcmp and (for nbfs_check) an allocator.
 * The block cache is write-through, so the on-disk state is always current
 * after every call (except the superblock counters, which are flushed at the
 * end of every mutating public operation).
 */
#if __has_include("fs/nbfs.h")
#include "fs/nbfs.h"
#else
#include "nbfs.h"        /* flat folder, e.g. inside an Xcode project */
#endif

#ifdef NIMBUS_KERNEL
#include "kernel/string.h"
#include "kernel/heap.h"
#include "kernel/kprintf.h"
#define nb_alloc(n) kzalloc(n)
#define nb_free(p) kfree(p)
#else
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define nb_alloc(n) calloc(1, n)
#define nb_free(p) free(p)
#endif

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

static u32 now(nbfs_t *fs) { return fs->now ? fs->now() : 0; }

/* ------------------------------------------------------------------------
 * block I/O with a small direct-mapped write-through cache
 * ---------------------------------------------------------------------- */
static int bread(nbfs_t *fs, u32 blk, void *buf)
{
    if (blk >= fs->sb.total_blocks) return NBFS_EIO;
    struct nbfs_cache_entry *e = &fs->cache[blk % NBFS_CACHE_SIZE];
    if (!e->valid || e->block != blk) {
        if (fs->dev.read(fs->dev.ctx, blk * NBFS_SECT_PER_BLK, NBFS_SECT_PER_BLK, e->data) != 0) {
            e->valid = false;
            return NBFS_EIO;
        }
        e->block = blk;
        e->valid = true;
    }
    memcpy(buf, e->data, NBFS_BLOCK);
    return NBFS_OK;
}

static int bwrite(nbfs_t *fs, u32 blk, const void *buf)
{
    if (blk >= fs->sb.total_blocks) return NBFS_EIO;
    struct nbfs_cache_entry *e = &fs->cache[blk % NBFS_CACHE_SIZE];
    if (fs->dev.write(fs->dev.ctx, blk * NBFS_SECT_PER_BLK, NBFS_SECT_PER_BLK, buf) != 0) {
        e->valid = false;
        return NBFS_EIO;
    }
    memcpy(e->data, buf, NBFS_BLOCK);
    e->block = blk;
    e->valid = true;
    return NBFS_OK;
}

static int write_super(nbfs_t *fs)
{
    u8 buf[NBFS_BLOCK];
    memset(buf, 0, sizeof buf);
    memcpy(buf, &fs->sb, sizeof fs->sb);
    int r = bwrite(fs, 1, buf);
    if (r == 0) fs->sb_dirty = false;
    return r;
}

int nbfs_sync(nbfs_t *fs) { return fs->sb_dirty ? write_super(fs) : NBFS_OK; }

/* ------------------------------------------------------------------------
 * bitmaps
 * ---------------------------------------------------------------------- */
#define BITS_PER_BLK (NBFS_BLOCK * 8)

/* Allocate a clear bit; returns its index or 0 (bit 0 is always in use in
 * both bitmaps, so 0 can signal failure). */
static u32 alloc_bit(nbfs_t *fs, u32 bm_start, u32 bm_blocks, u32 nbits, u32 *hint)
{
    u8 buf[NBFS_BLOCK];
    u32 start_blk = (*hint / BITS_PER_BLK) % bm_blocks;
    for (u32 n = 0; n < bm_blocks; n++) {
        u32 b = (start_blk + n) % bm_blocks;
        if (bread(fs, bm_start + b, buf) != 0) return 0;
        for (u32 i = 0; i < NBFS_BLOCK; i++) {
            if (buf[i] == 0xFF) continue;
            for (u32 bit = 0; bit < 8; bit++) {
                if (buf[i] & (1 << bit)) continue;
                u32 idx = b * BITS_PER_BLK + i * 8 + bit;
                if (idx >= nbits || idx == 0) continue;
                buf[i] |= 1 << bit;
                if (bwrite(fs, bm_start + b, buf) != 0) return 0;
                *hint = idx;
                return idx;
            }
        }
    }
    return 0;
}

static int free_bit(nbfs_t *fs, u32 bm_start, u32 idx)
{
    u8 buf[NBFS_BLOCK];
    u32 b = idx / BITS_PER_BLK, off = idx % BITS_PER_BLK;
    int r = bread(fs, bm_start + b, buf);
    if (r) return r;
    buf[off / 8] &= ~(1 << (off % 8));
    return bwrite(fs, bm_start + b, buf);
}

static int test_bit(nbfs_t *fs, u32 bm_start, u32 idx)
{
    u8 buf[NBFS_BLOCK];
    u32 b = idx / BITS_PER_BLK, off = idx % BITS_PER_BLK;
    if (bread(fs, bm_start + b, buf)) return -1;
    return (buf[off / 8] >> (off % 8)) & 1;
}

static u32 alloc_block(nbfs_t *fs, bool zero)
{
    if (fs->sb.free_blocks == 0) return 0;
    u32 b = alloc_bit(fs, fs->sb.block_bitmap_start, fs->sb.block_bitmap_blocks,
                      fs->sb.total_blocks, &fs->block_hint);
    if (!b) return 0;
    if (b < fs->sb.data_start) return 0;   /* corrupted bitmap: never hand out metadata */
    fs->sb.free_blocks--;
    fs->sb_dirty = true;
    if (zero) {
        u8 z[NBFS_BLOCK];
        memset(z, 0, sizeof z);
        if (bwrite(fs, b, z) != 0) return 0;
    }
    return b;
}

static void free_block(nbfs_t *fs, u32 b)
{
    if (b < fs->sb.data_start || b >= fs->sb.total_blocks) return;
    if (free_bit(fs, fs->sb.block_bitmap_start, b) == 0) {
        fs->sb.free_blocks++;
        fs->sb_dirty = true;
    }
}

/* ------------------------------------------------------------------------
 * inodes
 * ---------------------------------------------------------------------- */
static int iget(nbfs_t *fs, u32 ino, struct nbfs_inode *out)
{
    u8 buf[NBFS_BLOCK];
    if (ino == 0 || ino >= fs->sb.inode_count) return NBFS_EINVAL;
    int r = bread(fs, fs->sb.inode_table_start + ino / NBFS_INODES_PER_BLK, buf);
    if (r) return r;
    memcpy(out, buf + (ino % NBFS_INODES_PER_BLK) * NBFS_INODE_SIZE, sizeof *out);
    return NBFS_OK;
}

static int iput(nbfs_t *fs, u32 ino, const struct nbfs_inode *in)
{
    u8 buf[NBFS_BLOCK];
    if (ino == 0 || ino >= fs->sb.inode_count) return NBFS_EINVAL;
    u32 blk = fs->sb.inode_table_start + ino / NBFS_INODES_PER_BLK;
    int r = bread(fs, blk, buf);
    if (r) return r;
    memcpy(buf + (ino % NBFS_INODES_PER_BLK) * NBFS_INODE_SIZE, in, sizeof *in);
    return bwrite(fs, blk, buf);
}

static u32 alloc_inode(nbfs_t *fs)
{
    if (fs->sb.free_inodes == 0) return 0;
    u32 i = alloc_bit(fs, fs->sb.inode_bitmap_start, fs->sb.inode_bitmap_blocks,
                      fs->sb.inode_count, &fs->inode_hint);
    if (i) { fs->sb.free_inodes--; fs->sb_dirty = true; }
    return i;
}

static void free_inode(nbfs_t *fs, u32 ino)
{
    struct nbfs_inode z;
    memset(&z, 0, sizeof z);
    iput(fs, ino, &z);
    if (free_bit(fs, fs->sb.inode_bitmap_start, ino) == 0) {
        fs->sb.free_inodes++;
        fs->sb_dirty = true;
    }
}

/* Look up (and optionally allocate) entry idx of the pointer block whose
 * number is stored in *slot.  *slot itself is allocated on demand. */
static int map_indirect(nbfs_t *fs, u32 *slot, u32 idx, bool alloc, bool zero_leaf, u32 *out)
{
    u32 ptrs[NBFS_PTRS];
    if (!*slot) {
        if (!alloc) { *out = 0; return NBFS_OK; }
        *slot = alloc_block(fs, true);
        if (!*slot) return NBFS_ENOSPC;
    }
    int r = bread(fs, *slot, ptrs);
    if (r) return r;
    if (!ptrs[idx] && alloc) {
        ptrs[idx] = alloc_block(fs, zero_leaf);
        if (!ptrs[idx]) return NBFS_ENOSPC;
        r = bwrite(fs, *slot, ptrs);
        if (r) return r;
    }
    *out = ptrs[idx];
    return NBFS_OK;
}

/* Map file block fb of inode `in` to a disk block.  With alloc, missing
 * blocks are allocated (the caller must write the inode back). */
static int bmap(nbfs_t *fs, struct nbfs_inode *in, u32 fb, bool alloc, bool zero_leaf, u32 *out)
{
    if (fb < NBFS_NDIRECT) {
        if (!in->direct[fb] && alloc) {
            in->direct[fb] = alloc_block(fs, zero_leaf);
            if (!in->direct[fb]) return NBFS_ENOSPC;
        }
        *out = in->direct[fb];
        return NBFS_OK;
    }
    fb -= NBFS_NDIRECT;
    if (fb < NBFS_PTRS) {
        u32 slot = in->indirect;
        int r = map_indirect(fs, &slot, fb, alloc, zero_leaf, out);
        in->indirect = slot;
        return r;
    }
    fb -= NBFS_PTRS;
    if (fb < NBFS_PTRS * NBFS_PTRS) {
        u32 l1, slot = in->dindirect;
        int r = map_indirect(fs, &slot, fb / NBFS_PTRS, alloc, true, &l1);
        in->dindirect = slot;
        if (r) return r;
        if (!l1) { *out = 0; return NBFS_OK; }
        return map_indirect(fs, &l1, fb % NBFS_PTRS, alloc, zero_leaf, out);
    }
    return NBFS_ENOSPC;    /* beyond the maximum file size */
}

static int inode_read(nbfs_t *fs, struct nbfs_inode *in, u32 off, void *buf, u32 len)
{
    u8 tmp[NBFS_BLOCK];
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;
    u32 done = 0;
    while (done < len) {
        u32 pos = off + done, fb = pos / NBFS_BLOCK, boff = pos % NBFS_BLOCK;
        u32 n = NBFS_BLOCK - boff;
        if (n > len - done) n = len - done;
        u32 pb;
        int r = bmap(fs, in, fb, false, false, &pb);
        if (r) return r;
        if (!pb) memset((u8 *)buf + done, 0, n);   /* hole */
        else {
            r = bread(fs, pb, tmp);
            if (r) return r;
            memcpy((u8 *)buf + done, tmp + boff, n);
        }
        done += n;
    }
    return (int)done;
}

static int inode_write(nbfs_t *fs, u32 ino, struct nbfs_inode *in, u32 off, const void *buf, u32 len)
{
    u8 tmp[NBFS_BLOCK];
    u32 done = 0;
    int err = 0;
    if ((uint64_t)off + len > (uint64_t)NBFS_MAX_FILE_BLOCKS * NBFS_BLOCK) return NBFS_ENOSPC;
    while (done < len) {
        u32 pos = off + done, fb = pos / NBFS_BLOCK, boff = pos % NBFS_BLOCK;
        u32 n = NBFS_BLOCK - boff;
        if (n > len - done) n = len - done;
        bool full = (boff == 0 && n == NBFS_BLOCK);
        u32 pb;
        err = bmap(fs, in, fb, true, !full, &pb);
        if (err) break;
        if (!full) {
            err = bread(fs, pb, tmp);
            if (err) break;
        }
        memcpy(tmp + boff, (const u8 *)buf + done, n);
        err = bwrite(fs, pb, tmp);
        if (err) break;
        done += n;
    }
    if (off + done > in->size) in->size = off + done;
    if (done) in->mtime = now(fs);
    int r = iput(fs, ino, in);
    if (done) return (int)done;
    return err ? err : r;
}

/* Free all data blocks at or beyond file block `keep`. */
static void free_blocks_from(nbfs_t *fs, struct nbfs_inode *in, u32 keep)
{
    for (u32 i = keep; i < NBFS_NDIRECT; i++)
        if (in->direct[i]) { free_block(fs, in->direct[i]); in->direct[i] = 0; }

    if (in->indirect) {
        u32 p[NBFS_PTRS];
        if (bread(fs, in->indirect, p) == 0) {
            bool changed = false;
            for (u32 i = 0; i < NBFS_PTRS; i++)
                if (NBFS_NDIRECT + i >= keep && p[i]) { free_block(fs, p[i]); p[i] = 0; changed = true; }
            if (keep <= NBFS_NDIRECT) { free_block(fs, in->indirect); in->indirect = 0; }
            else if (changed) bwrite(fs, in->indirect, p);
        }
    }
    if (in->dindirect) {
        u32 l1[NBFS_PTRS];
        if (bread(fs, in->dindirect, l1) == 0) {
            bool l1_changed = false;
            for (u32 i = 0; i < NBFS_PTRS; i++) {
                if (!l1[i]) continue;
                u32 base = NBFS_NDIRECT + NBFS_PTRS + i * NBFS_PTRS;
                if (base + NBFS_PTRS <= keep) continue;   /* fully kept */
                u32 l2[NBFS_PTRS];
                if (bread(fs, l1[i], l2) != 0) continue;
                bool changed = false;
                for (u32 j = 0; j < NBFS_PTRS; j++)
                    if (base + j >= keep && l2[j]) { free_block(fs, l2[j]); l2[j] = 0; changed = true; }
                if (base >= keep) { free_block(fs, l1[i]); l1[i] = 0; l1_changed = true; }
                else if (changed) bwrite(fs, l1[i], l2);
            }
            if (keep <= NBFS_NDIRECT + NBFS_PTRS) { free_block(fs, in->dindirect); in->dindirect = 0; }
            else if (l1_changed) bwrite(fs, in->dindirect, l1);
        }
    }
}

static int inode_truncate(nbfs_t *fs, u32 ino, struct nbfs_inode *in, u32 size)
{
    if (size < in->size) {
        u32 keep = (size + NBFS_BLOCK - 1) / NBFS_BLOCK;
        free_blocks_from(fs, in, keep);
        /* zero the tail of the last partial block so a later extension
         * reads zeros rather than stale data */
        if (size % NBFS_BLOCK) {
            u32 pb;
            if (bmap(fs, in, size / NBFS_BLOCK, false, false, &pb) == 0 && pb) {
                u8 tmp[NBFS_BLOCK];
                if (bread(fs, pb, tmp) == 0) {
                    memset(tmp + size % NBFS_BLOCK, 0, NBFS_BLOCK - size % NBFS_BLOCK);
                    bwrite(fs, pb, tmp);
                }
            }
        }
    }
    in->size = size;
    in->mtime = now(fs);
    return iput(fs, ino, in);
}

/* ------------------------------------------------------------------------
 * directories
 * ---------------------------------------------------------------------- */
#define ENTRIES_PER_BLK (NBFS_BLOCK / NBFS_DIRENT_SIZE)

static int dir_find(nbfs_t *fs, u32 dir, const char *name, struct nbfs_dirent *found, u32 *off_out)
{
    struct nbfs_inode din;
    struct nbfs_dirent ents[ENTRIES_PER_BLK];
    int r = iget(fs, dir, &din);
    if (r) return r;
    if (din.type != NBFS_TYPE_DIR) return NBFS_ENOTDIR;
    u32 len = strlen(name);
    for (u32 off = 0; off < din.size; off += NBFS_BLOCK) {
        int n = inode_read(fs, &din, off, ents, NBFS_BLOCK);
        if (n < 0) return n;
        for (int i = 0; i < n / NBFS_DIRENT_SIZE; i++) {
            if (ents[i].inode && ents[i].name_len == len && memcmp(ents[i].name, name, len) == 0) {
                if (found) *found = ents[i];
                if (off_out) *off_out = off + i * NBFS_DIRENT_SIZE;
                return NBFS_OK;
            }
        }
    }
    return NBFS_ENOENT;
}

static int dir_add(nbfs_t *fs, u32 dir, const char *name, u32 ino, u32 type)
{
    struct nbfs_inode din;
    struct nbfs_dirent ents[ENTRIES_PER_BLK];
    int r = iget(fs, dir, &din);
    if (r) return r;
    u32 slot = din.size;
    for (u32 off = 0; off < din.size && slot == din.size; off += NBFS_BLOCK) {
        int n = inode_read(fs, &din, off, ents, NBFS_BLOCK);
        if (n < 0) return n;
        for (int i = 0; i < n / NBFS_DIRENT_SIZE; i++)
            if (!ents[i].inode) { slot = off + i * NBFS_DIRENT_SIZE; break; }
    }
    struct nbfs_dirent e;
    memset(&e, 0, sizeof e);
    e.inode = ino;
    e.type = (u8)type;
    e.name_len = (u8)strlen(name);
    memcpy(e.name, name, e.name_len);
    r = inode_write(fs, dir, &din, slot, &e, sizeof e);
    return r < 0 ? r : NBFS_OK;
}

static int dir_remove(nbfs_t *fs, u32 dir, const char *name)
{
    u32 off;
    int r = dir_find(fs, dir, name, NULL, &off);
    if (r) return r;
    struct nbfs_inode din;
    r = iget(fs, dir, &din);
    if (r) return r;
    struct nbfs_dirent z;
    memset(&z, 0, sizeof z);
    r = inode_write(fs, dir, &din, off, &z, sizeof z);
    if (r < 0) return r;
    /* shrink the directory if trailing entries are free */
    struct nbfs_dirent e;
    u32 new_size = din.size;
    while (new_size >= NBFS_DIRENT_SIZE) {
        if (inode_read(fs, &din, new_size - NBFS_DIRENT_SIZE, &e, sizeof e) != sizeof e || e.inode) break;
        new_size -= NBFS_DIRENT_SIZE;
    }
    return new_size == din.size ? NBFS_OK : inode_truncate(fs, dir, &din, new_size);
}

static bool dir_is_empty(nbfs_t *fs, struct nbfs_inode *din)
{
    struct nbfs_dirent ents[ENTRIES_PER_BLK];
    for (u32 off = 0; off < din->size; off += NBFS_BLOCK) {
        int n = inode_read(fs, din, off, ents, NBFS_BLOCK);
        if (n < 0) return false;
        for (int i = 0; i < n / NBFS_DIRENT_SIZE; i++) if (ents[i].inode) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * path helpers
 * ---------------------------------------------------------------------- */
void nbfs_path_normalize(const char *in, char *out, int max)
{
    /* stack of component start offsets in out */
    int stack[64], depth = 0, o = 0;
    if (max < 2) { if (max) out[0] = 0; return; }
    out[o++] = '/';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (depth > 0) o = stack[--depth];
            continue;
        }
        if (depth >= 64) continue;
        int start = o;
        if (o > 1) { if (o < max - 1) out[o++] = '/'; }
        stack[depth++] = start;
        for (int i = 0; i < len && o < max - 1; i++) out[o++] = s[i];
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
}

void nbfs_path_join(const char *dir, const char *name, char *out, int max)
{
    char tmp[NBFS_PATH_MAX * 2];
    int n = 0;
    if (name[0] == '/') {
        nbfs_path_normalize(name, out, max);
        return;
    }
    for (const char *s = dir; *s && n < (int)sizeof tmp - 2; s++) tmp[n++] = *s;
    tmp[n++] = '/';
    for (const char *s = name; *s && n < (int)sizeof tmp - 1; s++) tmp[n++] = *s;
    tmp[n] = 0;
    nbfs_path_normalize(tmp, out, max);
}

void nbfs_path_parent(const char *path, char *out, int max)
{
    char tmp[NBFS_PATH_MAX * 2];
    int n = 0;
    for (const char *s = path; *s && n < (int)sizeof tmp - 4; s++) tmp[n++] = *s;
    memcpy(tmp + n, "/..", 4);
    nbfs_path_normalize(tmp, out, max);
}

const char *nbfs_path_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) b = p + 1;
    return b;
}

static int valid_name(const char *name)
{
    u32 len = strlen(name);
    if (len == 0) return NBFS_EINVAL;
    if (len > NBFS_NAME_MAX) return NBFS_ENAMETOOLONG;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return NBFS_EINVAL;
    for (const char *p = name; *p; p++)
        if (*p == '/' || (u8)*p < 32) return NBFS_EINVAL;
    return NBFS_OK;
}

/* Resolve a normalized absolute path. */
static int resolve(nbfs_t *fs, const char *norm, u32 *ino)
{
    u32 cur = NBFS_ROOT_INO;
    const char *p = norm;
    char name[NBFS_NAME_MAX + 1];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int len = 0;
        while (*p && *p != '/') {
            if (len > NBFS_NAME_MAX) return NBFS_ENAMETOOLONG;
            name[len++] = *p++;
        }
        name[len] = 0;
        struct nbfs_dirent e;
        int r = dir_find(fs, cur, name, &e, NULL);
        if (r) return r;
        cur = e.inode;
    }
    *ino = cur;
    return NBFS_OK;
}

/* Split a path into (parent inode, final name). */
static int resolve_parent(nbfs_t *fs, const char *path, u32 *parent, char *name)
{
    char norm[NBFS_PATH_MAX], par[NBFS_PATH_MAX];
    nbfs_path_normalize(path, norm, sizeof norm);
    if (!strcmp(norm, "/")) return NBFS_EINVAL;
    const char *base = nbfs_path_basename(norm);
    if (strlen(base) > NBFS_NAME_MAX) return NBFS_ENAMETOOLONG;
    strcpy(name, base);
    int r = valid_name(name);
    if (r) return r;
    nbfs_path_parent(norm, par, sizeof par);
    r = resolve(fs, par, parent);
    if (r) return r;
    struct nbfs_inode pin;
    r = iget(fs, *parent, &pin);
    if (r) return r;
    return pin.type == NBFS_TYPE_DIR ? NBFS_OK : NBFS_ENOTDIR;
}

/* ------------------------------------------------------------------------
 * public API
 * ---------------------------------------------------------------------- */
#define FINISH(fs, r) do { int _s = nbfs_sync(fs); return (r) < 0 ? (r) : (_s ? _s : (r)); } while (0)

int nbfs_format(struct nbfs_blkdev *dev, const char *label, uint32_t when)
{
    u8 buf[NBFS_BLOCK];
    struct nbfs_super sb;
    memset(&sb, 0, sizeof sb);
    u32 total = dev->sectors / NBFS_SECT_PER_BLK;
    if (total < 64) return NBFS_EINVAL;
    sb.magic = NBFS_MAGIC;
    sb.version = NBFS_VERSION;
    sb.block_size = NBFS_BLOCK;
    sb.total_blocks = total;
    u32 inodes = total / 4;
    if (inodes < 64) inodes = 64;
    inodes = (inodes + NBFS_INODES_PER_BLK - 1) / NBFS_INODES_PER_BLK * NBFS_INODES_PER_BLK;
    sb.inode_count = inodes;
    sb.block_bitmap_start = 2;
    sb.block_bitmap_blocks = (total + BITS_PER_BLK - 1) / BITS_PER_BLK;
    sb.inode_bitmap_start = sb.block_bitmap_start + sb.block_bitmap_blocks;
    sb.inode_bitmap_blocks = (inodes + BITS_PER_BLK - 1) / BITS_PER_BLK;
    sb.inode_table_start = sb.inode_bitmap_start + sb.inode_bitmap_blocks;
    sb.inode_table_blocks = inodes / NBFS_INODES_PER_BLK;
    sb.data_start = sb.inode_table_start + sb.inode_table_blocks;
    if (sb.data_start + 16 > total) return NBFS_ENOSPC;
    sb.free_blocks = total - sb.data_start;
    sb.free_inodes = inodes - 2;           /* inode 0 reserved, inode 1 = root */
    sb.root_inode = NBFS_ROOT_INO;
    sb.created = when;
    for (int i = 0; label && label[i] && i < 31; i++) sb.label[i] = label[i];

#define WBLK(b, d) do { if (dev->write(dev->ctx, (b) * NBFS_SECT_PER_BLK, NBFS_SECT_PER_BLK, d)) return NBFS_EIO; } while (0)
    memset(buf, 0, sizeof buf);
    WBLK(0, buf);
    /* block bitmap: metadata blocks and bits past the end are "used" */
    for (u32 b = 0; b < sb.block_bitmap_blocks; b++) {
        memset(buf, 0, sizeof buf);
        for (u32 i = 0; i < BITS_PER_BLK; i++) {
            u32 idx = b * BITS_PER_BLK + i;
            if (idx < sb.data_start || idx >= total) buf[i / 8] |= 1 << (i % 8);
        }
        WBLK(sb.block_bitmap_start + b, buf);
    }
    for (u32 b = 0; b < sb.inode_bitmap_blocks; b++) {
        memset(buf, 0, sizeof buf);
        for (u32 i = 0; i < BITS_PER_BLK; i++) {
            u32 idx = b * BITS_PER_BLK + i;
            if (idx <= NBFS_ROOT_INO || idx >= inodes) buf[i / 8] |= 1 << (i % 8);
        }
        WBLK(sb.inode_bitmap_start + b, buf);
    }
    /* inode table: zeroed, then the root directory */
    for (u32 b = 0; b < sb.inode_table_blocks; b++) {
        memset(buf, 0, sizeof buf);
        if (b == 0) {
            struct nbfs_inode *root = (struct nbfs_inode *)(buf + NBFS_ROOT_INO * NBFS_INODE_SIZE);
            root->type = NBFS_TYPE_DIR;
            root->links = 1;
            root->ctime = root->mtime = when;
            root->parent = NBFS_ROOT_INO;
        }
        WBLK(sb.inode_table_start + b, buf);
    }
    memset(buf, 0, sizeof buf);
    memcpy(buf, &sb, sizeof sb);
    WBLK(1, buf);
#undef WBLK
    return NBFS_OK;
}

int nbfs_mount(nbfs_t *fs, struct nbfs_blkdev *dev)
{
    u8 buf[NBFS_BLOCK];
    uint32_t (*keep_now)(void) = fs->now;
    memset(fs, 0, sizeof *fs);
    fs->now = keep_now;
    fs->dev = *dev;
    if (dev->read(dev->ctx, 1 * NBFS_SECT_PER_BLK, NBFS_SECT_PER_BLK, buf) != 0) return NBFS_EIO;
    memcpy(&fs->sb, buf, sizeof fs->sb);
    if (fs->sb.magic != NBFS_MAGIC || fs->sb.version != NBFS_VERSION || fs->sb.block_size != NBFS_BLOCK)
        return NBFS_EBADFS;
    if (fs->sb.total_blocks > dev->sectors / NBFS_SECT_PER_BLK || fs->sb.data_start >= fs->sb.total_blocks)
        return NBFS_EBADFS;
    fs->block_hint = fs->sb.data_start;
    fs->inode_hint = 2;
    return NBFS_OK;
}

void nbfs_statfs(nbfs_t *fs, uint32_t *total, uint32_t *free_bytes)
{
    if (total) *total = (fs->sb.total_blocks - fs->sb.data_start) * NBFS_BLOCK;
    if (free_bytes) *free_bytes = fs->sb.free_blocks * NBFS_BLOCK;
}

int nbfs_lookup(nbfs_t *fs, const char *path, uint32_t *ino)
{
    char norm[NBFS_PATH_MAX];
    nbfs_path_normalize(path, norm, sizeof norm);
    return resolve(fs, norm, ino);
}

int nbfs_stat_inode(nbfs_t *fs, uint32_t ino, struct nbfs_stat *st)
{
    struct nbfs_inode in;
    int r = iget(fs, ino, &in);
    if (r) return r;
    if (in.type == NBFS_TYPE_FREE) return NBFS_ENOENT;
    st->inode = ino;
    st->type = in.type;
    st->size = in.size;
    st->ctime = in.ctime;
    st->mtime = in.mtime;
    st->parent = in.parent;
    return NBFS_OK;
}

int nbfs_stat(nbfs_t *fs, const char *path, struct nbfs_stat *st)
{
    u32 ino;
    int r = nbfs_lookup(fs, path, &ino);
    return r ? r : nbfs_stat_inode(fs, ino, st);
}

int nbfs_create(nbfs_t *fs, const char *path, int type)
{
    u32 parent;
    char name[NBFS_NAME_MAX + 1];
    if (type != NBFS_TYPE_FILE && type != NBFS_TYPE_DIR) return NBFS_EINVAL;
    int r = resolve_parent(fs, path, &parent, name);
    if (r) return r;
    if (dir_find(fs, parent, name, NULL, NULL) == NBFS_OK) return NBFS_EEXIST;
    u32 ino = alloc_inode(fs);
    if (!ino) FINISH(fs, NBFS_ENOSPC);
    struct nbfs_inode in;
    memset(&in, 0, sizeof in);
    in.type = (u16)type;
    in.links = 1;
    in.ctime = in.mtime = now(fs);
    in.parent = parent;
    r = iput(fs, ino, &in);
    if (!r) r = dir_add(fs, parent, name, ino, type);
    if (r) { free_inode(fs, ino); FINISH(fs, r); }
    FINISH(fs, (int)ino);
}

int nbfs_mkdir(nbfs_t *fs, const char *path)
{
    int r = nbfs_create(fs, path, NBFS_TYPE_DIR);
    return r < 0 ? r : NBFS_OK;
}

int nbfs_read(nbfs_t *fs, uint32_t ino, uint32_t off, void *buf, uint32_t len)
{
    struct nbfs_inode in;
    int r = iget(fs, ino, &in);
    if (r) return r;
    if (in.type == NBFS_TYPE_DIR) return NBFS_EISDIR;
    if (in.type != NBFS_TYPE_FILE) return NBFS_ENOENT;
    return inode_read(fs, &in, off, buf, len);
}

int nbfs_write(nbfs_t *fs, uint32_t ino, uint32_t off, const void *buf, uint32_t len)
{
    struct nbfs_inode in;
    int r = iget(fs, ino, &in);
    if (r) return r;
    if (in.type == NBFS_TYPE_DIR) return NBFS_EISDIR;
    if (in.type != NBFS_TYPE_FILE) return NBFS_ENOENT;
    if (len == 0) return 0;
    r = inode_write(fs, ino, &in, off, buf, len);
    FINISH(fs, r);
}

int nbfs_truncate(nbfs_t *fs, uint32_t ino, uint32_t size)
{
    struct nbfs_inode in;
    int r = iget(fs, ino, &in);
    if (r) return r;
    if (in.type != NBFS_TYPE_FILE) return in.type == NBFS_TYPE_DIR ? NBFS_EISDIR : NBFS_ENOENT;
    r = inode_truncate(fs, ino, &in, size);
    FINISH(fs, r);
}

int nbfs_unlink(nbfs_t *fs, const char *path)
{
    u32 parent, ino;
    char name[NBFS_NAME_MAX + 1];
    int r = resolve_parent(fs, path, &parent, name);
    if (r) return r;
    struct nbfs_dirent e;
    r = dir_find(fs, parent, name, &e, NULL);
    if (r) return r;
    ino = e.inode;
    struct nbfs_inode in;
    r = iget(fs, ino, &in);
    if (r) return r;
    if (in.type == NBFS_TYPE_DIR && !dir_is_empty(fs, &in)) return NBFS_ENOTEMPTY;
    r = dir_remove(fs, parent, name);
    if (r) FINISH(fs, r);
    free_blocks_from(fs, &in, 0);
    free_inode(fs, ino);
    FINISH(fs, NBFS_OK);
}

int nbfs_remove_tree(nbfs_t *fs, const char *path)
{
    struct nbfs_stat st;
    char norm[NBFS_PATH_MAX];
    nbfs_path_normalize(path, norm, sizeof norm);
    int r = nbfs_stat(fs, norm, &st);
    if (r) return r;
    if (st.type == NBFS_TYPE_DIR) {
        for (;;) {
            uint32_t cookie = 0;
            struct nbfs_entry e;
            r = nbfs_readdir(fs, st.inode, &cookie, &e);
            if (r < 0) return r;
            if (r == 0) break;
            char child[NBFS_PATH_MAX];
            nbfs_path_join(norm, e.name, child, sizeof child);
            r = nbfs_remove_tree(fs, child);
            if (r) return r;
        }
    }
    return nbfs_unlink(fs, norm);
}

int nbfs_rename(nbfs_t *fs, const char *from, const char *to)
{
    u32 op, np;
    char oname[NBFS_NAME_MAX + 1], nname[NBFS_NAME_MAX + 1];
    int r = resolve_parent(fs, from, &op, oname);
    if (r) return r;
    struct nbfs_dirent e;
    r = dir_find(fs, op, oname, &e, NULL);
    if (r) return r;
    r = resolve_parent(fs, to, &np, nname);
    if (r) return r;
    struct nbfs_dirent existing;
    if (dir_find(fs, np, nname, &existing, NULL) == NBFS_OK)
        return existing.inode == e.inode ? NBFS_OK : NBFS_EEXIST;
    if (e.type == NBFS_TYPE_DIR) {
        /* refuse to move a directory into itself or one of its descendants */
        u32 p = np;
        for (int guard = 0; guard < 1024; guard++) {
            if (p == e.inode) return NBFS_EINVAL;
            if (p == NBFS_ROOT_INO) break;
            struct nbfs_inode pin;
            if (iget(fs, p, &pin)) break;
            p = pin.parent;
        }
    }
    r = dir_add(fs, np, nname, e.inode, e.type);
    if (r) FINISH(fs, r);
    r = dir_remove(fs, op, oname);
    if (r) FINISH(fs, r);
    struct nbfs_inode in;
    if (iget(fs, e.inode, &in) == 0) {
        in.parent = np;
        iput(fs, e.inode, &in);
    }
    FINISH(fs, NBFS_OK);
}

int nbfs_readdir(nbfs_t *fs, uint32_t dir, uint32_t *cookie, struct nbfs_entry *out)
{
    struct nbfs_inode din;
    int r = iget(fs, dir, &din);
    if (r) return r;
    if (din.type != NBFS_TYPE_DIR) return NBFS_ENOTDIR;
    while (*cookie + NBFS_DIRENT_SIZE <= din.size) {
        struct nbfs_dirent e;
        r = inode_read(fs, &din, *cookie, &e, sizeof e);
        if (r < 0) return r;
        *cookie += NBFS_DIRENT_SIZE;
        if (!e.inode) continue;
        out->inode = e.inode;
        out->type = e.type;
        u32 n = e.name_len > NBFS_NAME_MAX ? NBFS_NAME_MAX : e.name_len;
        memcpy(out->name, e.name, n);
        out->name[n] = 0;
        return 1;
    }
    return 0;
}

int nbfs_get_path(nbfs_t *fs, uint32_t ino, char *out, int max)
{
    char tmp[NBFS_PATH_MAX];
    int pos = sizeof tmp - 1;
    tmp[pos] = 0;
    for (int guard = 0; ino != NBFS_ROOT_INO && guard < 64; guard++) {
        struct nbfs_inode in;
        int r = iget(fs, ino, &in);
        if (r) return r;
        /* find our name in the parent */
        u32 cookie = 0;
        struct nbfs_entry e;
        bool found = false;
        while (nbfs_readdir(fs, in.parent, &cookie, &e) == 1)
            if (e.inode == ino) { found = true; break; }
        if (!found) return NBFS_ENOENT;
        int len = strlen(e.name);
        if (pos - len - 1 < 0) return NBFS_ENAMETOOLONG;
        pos -= len;
        memcpy(tmp + pos, e.name, len);
        tmp[--pos] = '/';
        ino = in.parent;
    }
    if (tmp[pos] == 0) tmp[--pos] = '/';
    int len = sizeof tmp - 1 - pos;
    if (len >= max) return NBFS_ENAMETOOLONG;
    memcpy(out, tmp + pos, len + 1);
    return NBFS_OK;
}

int nbfs_read_file(nbfs_t *fs, const char *path, void *buf, uint32_t max)
{
    u32 ino;
    int r = nbfs_lookup(fs, path, &ino);
    if (r) return r;
    return nbfs_read(fs, ino, 0, buf, max);
}

int nbfs_write_file(nbfs_t *fs, const char *path, const void *buf, uint32_t len)
{
    u32 ino;
    int r = nbfs_lookup(fs, path, &ino);
    if (r == NBFS_ENOENT) {
        r = nbfs_create(fs, path, NBFS_TYPE_FILE);
        if (r < 0) return r;
        ino = (u32)r;
    } else if (r) {
        return r;
    } else {
        r = nbfs_truncate(fs, ino, 0);
        if (r) return r;
    }
    if (len == 0) return 0;
    return nbfs_write(fs, ino, 0, buf, len);
}

const char *nbfs_strerror(int err)
{
    switch (err) {
    case NBFS_OK: return "success";
    case NBFS_ENOENT: return "no such file or directory";
    case NBFS_EIO: return "I/O error";
    case NBFS_EEXIST: return "already exists";
    case NBFS_ENOTDIR: return "not a directory";
    case NBFS_EISDIR: return "is a directory";
    case NBFS_EINVAL: return "invalid argument";
    case NBFS_ENOSPC: return "no space left on volume";
    case NBFS_ENAMETOOLONG: return "name too long";
    case NBFS_ENOTEMPTY: return "directory not empty";
    case NBFS_EBADFS: return "not a NimbusFS volume";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------------
 * consistency checker
 * ---------------------------------------------------------------------- */
struct check_ctx {
    nbfs_t *fs;
    u8 *seen_blocks, *seen_inodes;
    int problems;
    u32 used_blocks, used_inodes;
    void (*report)(const char *);
};

static void problem(struct check_ctx *c, const char *what, u32 a, u32 b)
{
    c->problems++;
    if (c->report) {
        char msg[128];
        /* tiny formatter to stay libc-free */
        int n = 0;
        for (const char *p = what; *p && n < 80; p++) msg[n++] = *p;
        u32 vals[2] = { a, b };
        for (int v = 0; v < 2; v++) {
            msg[n++] = ' ';
            char d[12]; int k = 0; u32 x = vals[v];
            do { d[k++] = '0' + x % 10; x /= 10; } while (x);
            while (k) msg[n++] = d[--k];
        }
        msg[n] = 0;
        c->report(msg);
    }
}

static void mark_block(struct check_ctx *c, u32 b, u32 ino)
{
    if (b < c->fs->sb.data_start || b >= c->fs->sb.total_blocks) { problem(c, "inode/bad block pointer", ino, b); return; }
    if (c->seen_blocks[b / 8] & (1 << (b % 8))) { problem(c, "inode/doubly used block", ino, b); return; }
    c->seen_blocks[b / 8] |= 1 << (b % 8);
    c->used_blocks++;
}

static void mark_ptr_block(struct check_ctx *c, u32 b, u32 ino, int level)
{
    u32 p[NBFS_PTRS];
    mark_block(c, b, ino);
    if (bread(c->fs, b, p)) { problem(c, "inode/unreadable pointer block", ino, b); return; }
    for (int i = 0; i < NBFS_PTRS; i++) {
        if (!p[i]) continue;
        if (level > 1) mark_ptr_block(c, p[i], ino, level - 1);
        else mark_block(c, p[i], ino);
    }
}

static void check_inode(struct check_ctx *c, u32 ino, u32 parent, int depth)
{
    struct nbfs_inode in;
    if (ino == 0 || ino >= c->fs->sb.inode_count) { problem(c, "dirent/bad inode number", parent, ino); return; }
    if (c->seen_inodes[ino / 8] & (1 << (ino % 8))) { problem(c, "inode/linked twice", ino, parent); return; }
    c->seen_inodes[ino / 8] |= 1 << (ino % 8);
    c->used_inodes++;
    if (iget(c->fs, ino, &in)) { problem(c, "inode/unreadable", ino, 0); return; }
    if (in.type != NBFS_TYPE_FILE && in.type != NBFS_TYPE_DIR) { problem(c, "inode/bad type", ino, in.type); return; }
    if (in.parent != parent) problem(c, "inode/wrong parent", ino, in.parent);
    if (test_bit(c->fs, c->fs->sb.inode_bitmap_start, ino) != 1) problem(c, "inode/not marked in bitmap", ino, 0);
    for (int i = 0; i < NBFS_NDIRECT; i++) if (in.direct[i]) mark_block(c, in.direct[i], ino);
    if (in.indirect) mark_ptr_block(c, in.indirect, ino, 1);
    if (in.dindirect) mark_ptr_block(c, in.dindirect, ino, 2);
    if (in.type == NBFS_TYPE_DIR) {
        if (depth > 64) { problem(c, "dir/too deep", ino, depth); return; }
        if (in.size % NBFS_DIRENT_SIZE) problem(c, "dir/bad size", ino, in.size);
        u32 cookie = 0;
        struct nbfs_entry e;
        while (nbfs_readdir(c->fs, ino, &cookie, &e) == 1) {
            if (valid_name(e.name) != NBFS_OK) problem(c, "dirent/bad name in dir", ino, e.inode);
            struct nbfs_inode child;
            if (iget(c->fs, e.inode, &child) == 0 && child.type != e.type) problem(c, "dirent/type mismatch", ino, e.inode);
            check_inode(c, e.inode, ino, depth + 1);
        }
    }
}

int nbfs_check(nbfs_t *fs, void (*report)(const char *msg))
{
    struct check_ctx c;
    memset(&c, 0, sizeof c);
    c.fs = fs;
    c.report = report;
    c.seen_blocks = nb_alloc(fs->sb.total_blocks / 8 + 1);
    c.seen_inodes = nb_alloc(fs->sb.inode_count / 8 + 1);
    if (!c.seen_blocks || !c.seen_inodes) { nb_free(c.seen_blocks); nb_free(c.seen_inodes); return 1; }
    check_inode(&c, NBFS_ROOT_INO, NBFS_ROOT_INO, 0);
    for (u32 b = fs->sb.data_start; b < fs->sb.total_blocks; b++) {
        int used = test_bit(fs, fs->sb.block_bitmap_start, b);
        int seen = (c.seen_blocks[b / 8] >> (b % 8)) & 1;
        if (used == 1 && !seen) problem(&c, "bitmap/leaked block", b, 0);
        if (used == 0 && seen) problem(&c, "bitmap/used block marked free", b, 0);
    }
    for (u32 i = 2; i < fs->sb.inode_count; i++) {
        int used = test_bit(fs, fs->sb.inode_bitmap_start, i);
        int seen = (c.seen_inodes[i / 8] >> (i % 8)) & 1;
        if (used == 1 && !seen) problem(&c, "bitmap/orphan inode", i, 0);
    }
    u32 free_b = fs->sb.total_blocks - fs->sb.data_start - c.used_blocks;
    if (free_b != fs->sb.free_blocks) problem(&c, "super/free block count (stored, actual)", fs->sb.free_blocks, free_b);
    u32 free_i = fs->sb.inode_count - 1 - c.used_inodes;
    if (free_i != fs->sb.free_inodes) problem(&c, "super/free inode count (stored, actual)", fs->sb.free_inodes, free_i);
    nb_free(c.seen_blocks);
    nb_free(c.seen_inodes);
    return c.problems;
}
