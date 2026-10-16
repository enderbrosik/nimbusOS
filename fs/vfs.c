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
#include "kernel/vmm.h"

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

/* ---- live CD RAM disk (whole disk image loaded by stage 2) ---- */
static u8 *ramdisk;
static u32 ramdisk_sectors;

bool vfs_booted_from_ramdisk(void) { return ramdisk != NULL; }

static int ram_read(void *ctx, u32 lba, u32 count, void *buf)
{
    struct part_ctx *p = ctx;
    if (lba + count > p->sectors) return -1;
    memcpy(buf, ramdisk + (u64)(p->lba + lba) * 512, count * 512);
    return 0;
}

static int ram_write(void *ctx, u32 lba, u32 count, const void *buf)
{
    struct part_ctx *p = ctx;
    if (lba + count > p->sectors) return -1;
    memcpy(ramdisk + (u64)(p->lba + lba) * 512, buf, count * 512);
    return 0;
}

int boot_disk_read(u32 lba, u32 count, void *buf)
{
    if (ramdisk) {
        if (lba + count > ramdisk_sectors) return -1;
        memcpy(buf, ramdisk + (u64)lba * 512, count * 512);
        return 0;
    }
    return ata_read(g_root->drive, lba, count, buf);
}

const char *boot_disk_name(void)
{
    return ramdisk ? "Live CD (RAM disk)" : g_ata[g_root->drive].name;
}

static bool mount_ramdisk(void)
{
    ramdisk = (u8 *)g_bootinfo.ramdisk_addr;
    ramdisk_sectors = g_bootinfo.ramdisk_sectors;
    vmm_identity_map(g_bootinfo.ramdisk_addr, ramdisk_sectors * 512);
    u8 *pe = ramdisk + 0x1BE;
    if (ramdisk[510] != 0x55 || ramdisk[511] != 0xAA || pe[4] != NBFS_PART_TYPE) return false;
    struct volume *v = kzalloc(sizeof *v);
    v->drive = -1;
    v->part_lba = *(u32 *)(pe + 8);
    v->part_sectors = *(u32 *)(pe + 12);
    struct part_ctx *p = kmalloc(sizeof *p);
    p->drive = -1; p->lba = v->part_lba; p->sectors = v->part_sectors;
    v->dev = (struct nbfs_blkdev){ p, ram_read, ram_write, v->part_sectors };
    v->fs.now = fs_now;
    if (nbfs_mount(&v->fs, &v->dev)) { kfree(p); kfree(v); return false; }
    g_root = v;
    u32 total, freeb;
    nbfs_statfs(ROOTFS, &total, &freeb);
    kprintf("[vfs ] live CD: mounted \"%s\" from RAM disk at %p: %u KiB, %u KiB free (changes are not saved)\n",
            ROOTFS->sb.label, ramdisk, total / 1024, freeb / 1024);
    return true;
}

bool vfs_init(void)
{
    if (g_bootinfo.ramdisk_addr && g_bootinfo.ramdisk_sectors) return mount_ramdisk();
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

static bool item_before(struct vfs_item *a, struct vfs_item *b)
{
    if (a->type != b->type) return a->type == NBFS_TYPE_DIR;
    return strcasecmp(a->name, b->name) < 0;
}

int vfs_list(const char *path, struct vfs_item **out)
{
    struct nbfs_stat st;
    int r = nbfs_stat(ROOTFS, path, &st);
    if (r) return r;
    if (st.type != NBFS_TYPE_DIR) return NBFS_ENOTDIR;
    int cap = 16, n = 0;
    struct vfs_item *items = kmalloc(cap * sizeof *items);
    u32 cookie = 0;
    struct nbfs_entry e;
    while ((r = nbfs_readdir(ROOTFS, st.inode, &cookie, &e)) == 1) {
        if (n == cap) {
            cap *= 2;
            items = krealloc(items, cap * sizeof *items);
        }
        struct nbfs_stat cs;
        memset(&items[n], 0, sizeof items[n]);
        strlcpy(items[n].name, e.name, sizeof items[n].name);
        items[n].type = e.type;
        if (nbfs_stat_inode(ROOTFS, e.inode, &cs) == 0) {
            items[n].size = cs.size;
            items[n].mtime = cs.mtime;
            items[n].ctime = cs.ctime;
        }
        n++;
    }
    for (int i = 1; i < n; i++) {           /* insertion sort */
        struct vfs_item t = items[i];
        int j = i - 1;
        while (j >= 0 && item_before(&t, &items[j])) { items[j + 1] = items[j]; j--; }
        items[j + 1] = t;
    }
    *out = items;
    return r < 0 ? r : n;
}
