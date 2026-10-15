/*
 * NimbusOS - volume management glue between the ATA driver and NimbusFS.
 *
 * The system volume is the NimbusFS partition (MBR type 0x4E) on the ATA
 * disk whose MBR disk signature matches the one stage 2 read from the BIOS
 * boot disk.  Only the GUI task touches the file system, so no locking is
 * needed beyond what the ATA driver does internally.
 */
#include "fs/vfs.h"
#include "drivers/ata.h"
#include "drivers/rtc.h"
#include "kernel/bootinfo.h"
#include "kernel/heap.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"

struct volume *g_root;

struct part_ctx { int drive; u32 lba, sectors; };

static int part_read(void *ctx, u32 lba, u32 count, void *buf)
{
    struct part_ctx *p = ctx;
    if (lba + count > p->sectors) return -1;
    return ata_read(p->drive, p->lba + lba, count, buf);
}

static int part_write(void *ctx, u32 lba, u32 count, const void *buf)
{
    struct part_ctx *p = ctx;
    if (lba + count > p->sectors) return -1;
    return ata_write(p->drive, p->lba + lba, count, buf);
}

void blkdev_for_partition(struct nbfs_blkdev *dev, int drive, u32 lba, u32 sectors)
{
    struct part_ctx *p = kmalloc(sizeof *p);
    p->drive = drive; p->lba = lba; p->sectors = sectors;
    dev->ctx = p;
    dev->read = part_read;
    dev->write = part_write;
    dev->sectors = sectors;
}

int boot_drive_index(void)
{
    static u8 mbr[512];
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        if (!g_ata[i].present || ata_read(i, 0, 1, mbr) != 0) continue;
        if (*(u32 *)(mbr + 0x1B8) == g_bootinfo.disk_signature) return i;
    }
    return -1;
}

bool find_nbfs_partition(int drive, u32 *lba, u32 *sectors)
{
    static u8 mbr[512];
    if (ata_read(drive, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
    for (int i = 0; i < 4; i++) {
        u8 *pe = mbr + 0x1BE + i * 16;
        if (pe[4] == NBFS_PART_TYPE) {
            *lba = *(u32 *)(pe + 8);
            *sectors = *(u32 *)(pe + 12);
            return true;
        }
    }
    return false;
}

static u32 fs_now(void) { return rtc_unix_time(); }

struct volume *volume_mount(int drive)
{
    u32 lba, sectors;
    if (!find_nbfs_partition(drive, &lba, &sectors)) return NULL;
    struct volume *v = kzalloc(sizeof *v);
    if (!v) return NULL;
    v->drive = drive;
    v->part_lba = lba;
    v->part_sectors = sectors;
    blkdev_for_partition(&v->dev, drive, lba, sectors);
    v->fs.now = fs_now;
    int r = nbfs_mount(&v->fs, &v->dev);
    if (r) {
        kprintf("[vfs ] mount of drive %d failed: %s\n", drive, nbfs_strerror(r));
        kfree(v->dev.ctx);
        kfree(v);
        return NULL;
    }
    return v;
}

void volume_unmount(struct volume *v)
{
    if (!v) return;
    nbfs_sync(&v->fs);
    kfree(v->dev.ctx);
    kfree(v);
}

bool vfs_init(void)
{
    int d = boot_drive_index();
    if (d < 0) {
        kprintf("[vfs ] boot disk not found by signature, trying drive 0\n");
        d = 0;
    }
    g_root = volume_mount(d);
    if (!g_root) return false;
    u32 total, freeb;
    nbfs_statfs(ROOTFS, &total, &freeb);
    kprintf("[vfs ] mounted \"%s\" from %s (LBA %u): %u KiB, %u KiB free\n",
            ROOTFS->sb.label, g_ata[d].name, g_root->part_lba, total / 1024, freeb / 1024);
    return true;
}

int vfs_read_all(const char *path, char **out, u32 *len)
{
    struct nbfs_stat st;
    int r = nbfs_stat(ROOTFS, path, &st);
    if (r) return r;
    if (st.type != NBFS_TYPE_FILE) return NBFS_EISDIR;
    char *buf = kmalloc(st.size + 1);
    if (!buf) return NBFS_ENOSPC;
    r = nbfs_read(ROOTFS, st.inode, 0, buf, st.size);
    if (r < 0) { kfree(buf); return r; }
    buf[r] = 0;
    *out = buf;
    if (len) *len = r;
    return NBFS_OK;
}

int vfs_write_all(const char *path, const void *buf, u32 len)
{
    int r = nbfs_write_file(ROOTFS, path, buf, len);
    return r < 0 ? r : NBFS_OK;
}

bool vfs_exists(const char *path)
{
    u32 ino;
    return nbfs_lookup(ROOTFS, path, &ino) == NBFS_OK;
}

bool vfs_is_dir(const char *path)
{
    struct nbfs_stat st;
    return nbfs_stat(ROOTFS, path, &st) == NBFS_OK && st.type == NBFS_TYPE_DIR;
}

int vfs_create_unique(const char *dir, const char *base, const char *ext, int type, char *out, int max)
{
    char name[NBFS_NAME_MAX + 1];
    for (int i = 1; i < 1000; i++) {
        if (i == 1) snprintf(name, sizeof name, "%s%s", base, ext);
        else snprintf(name, sizeof name, "%s (%d)%s", base, i, ext);
        nbfs_path_join(dir, name, out, max);
        if (vfs_exists(out)) continue;
        int r = nbfs_create(ROOTFS, out, type);
        return r < 0 ? r : NBFS_OK;
    }
    return NBFS_EEXIST;
}

void vfs_format_size(u32 bytes, char *out, int max)
{
    if (bytes < 1024) snprintf(out, max, "%u bytes", bytes);
    else if (bytes < 1024 * 1024) snprintf(out, max, "%u.%u KB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    else snprintf(out, max, "%u.%u MB", bytes / (1024 * 1024), (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
}

void vfs_format_time(u32 t, char *out, int max)
{
    struct rtc_time tm;
    unix_to_rtc(t, &tm);
    snprintf(out, max, "%04d-%02d-%02d %02d:%02d", tm.year, tm.month, tm.day, tm.hour, tm.minute);
}
