/*
 * NimbusOS - volume management glue between the ATA driver and NimbusFS.
 *
 * The system volume is the NimbusFS partition (MBR type 0x4E) on the ATA
 * disk whose MBR disk signature matches the one stage 2 read from the BIOS
 * boot disk.  Only the GUI task touches the file system, so no locking is
 * needed beyond what the ATA driver does internally.
 */
#include "fs/vfs.h"
#include "fs/fat32.h"
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
    ramdisk = (u8 *)(uptr)g_bootinfo.ramdisk_addr;
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

/* The first plugged-in FAT32 disk (a USB stick) appears as the folder /USB. */
bool vfs_is_usb(const char *p) { return !strncmp(p, "/USB", 4) && (p[4] == '/' || p[4] == 0); }
static const char *usb_sub(const char *p) { return p[4] ? p + 4 : "/"; }
static bool usb_ready(void) { return fat32_mounted() || fat32_mount() == 0; }

int vfs_read_all(const char *path, char **out, u32 *len)
{
    if (vfs_is_usb(path)) { if (!usb_ready()) return NBFS_ENOENT; u32 n; int r = fat32_read(usb_sub(path), out, &n); if (!r && len) *len = n; return r; }
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
    if (vfs_is_usb(path)) return usb_ready() ? fat32_write(usb_sub(path), buf, len) : NBFS_ENOENT;
    int r = nbfs_write_file(ROOTFS, path, buf, len);
    return r < 0 ? r : NBFS_OK;
}

bool vfs_exists(const char *path)
{
    if (vfs_is_usb(path)) { struct fat_item *it; if (!usb_ready()) return false; if (!usb_sub(path)[1]) return true; char par[NBFS_PATH_MAX]; nbfs_path_parent(usb_sub(path), par, sizeof par); int n = fat32_list(par, &it); for (int i = 0; i < n; i++) if (!strcasecmp(it[i].name, nbfs_path_basename(path))) { kfree(it); return true; } if (n > 0) kfree(it); return false; }
    u32 ino;
    return nbfs_lookup(ROOTFS, path, &ino) == NBFS_OK;
}

bool vfs_is_dir(const char *path)
{
    if (vfs_is_usb(path)) { struct fat_item *it; if (!usb_ready()) return false; if (!usb_sub(path)[1]) return true; int n = fat32_list(usb_sub(path), &it); if (n > 0) kfree(it); return n >= 0; }
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
        if (vfs_is_usb(out)) return type == NBFS_TYPE_DIR ? fat32_mkdir(usb_sub(out)) : fat32_write(usb_sub(out), "", 0);
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

static int usb_list(const char *path, struct vfs_item **out)
{
    if (!usb_ready()) return NBFS_ENOENT;
    struct fat_item *fi;
    int n = fat32_list(usb_sub(path), &fi);
    if (n < 0) return n;
    struct vfs_item *items = kmalloc((n ? n : 1) * sizeof *items);
    if (!items) { kfree(fi); return NBFS_ENOSPC; }
    for (int i = 0; i < n; i++) {
        memset(&items[i], 0, sizeof items[i]);
        strlcpy(items[i].name, fi[i].name, sizeof items[i].name);
        items[i].type = fi[i].dir ? NBFS_TYPE_DIR : NBFS_TYPE_FILE;
        items[i].size = fi[i].size;
        items[i].mtime = items[i].ctime = fi[i].mtime;
    }
    if (n > 0) kfree(fi);
    for (int i = 1; i < n; i++) {
        struct vfs_item t = items[i];
        int j = i - 1;
        while (j >= 0 && item_before(&t, &items[j])) { items[j + 1] = items[j]; j--; }
        items[j + 1] = t;
    }
    *out = items;
    return n;
}

int vfs_list(const char *path, struct vfs_item **out)
{
    if (vfs_is_usb(path)) return usb_list(path, out);
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
    if (!strcmp(path, "/") && usb_ready()) {          /* the USB disk is listed as a folder in the root */
        if (n == cap) items = krealloc(items, (cap + 1) * sizeof *items);
        memset(&items[n], 0, sizeof items[n]);
        strlcpy(items[n].name, "USB", sizeof items[n].name);
        items[n].type = NBFS_TYPE_DIR;
        items[n].mtime = items[n].ctime = rtc_unix_time();
        n++;
        for (int i = n - 1; i > 0 && item_before(&items[i], &items[i - 1]); i--) { struct vfs_item t = items[i]; items[i] = items[i - 1]; items[i - 1] = t; }
    }
    *out = items;
    return r < 0 ? r : n;
}

int vfs_mkdir(const char *path) { return vfs_is_usb(path) ? (usb_ready() ? fat32_mkdir(usb_sub(path)) : NBFS_ENOENT) : nbfs_mkdir(ROOTFS, path); }

int vfs_rename(const char *from, const char *to)
{
    if (vfs_is_usb(from) != vfs_is_usb(to)) return NBFS_EINVAL;
    if (vfs_is_usb(from)) return usb_ready() ? fat32_rename(usb_sub(from), usb_sub(to)) : NBFS_ENOENT;
    return nbfs_rename(ROOTFS, from, to);
}

int vfs_remove_tree(const char *path)
{
    if (!vfs_is_usb(path)) return nbfs_remove_tree(ROOTFS, path);
    if (!usb_ready()) return NBFS_ENOENT;
    if (vfs_is_dir(path)) {
        struct vfs_item *items;
        int n = usb_list(path, &items), r = 0;
        for (int i = 0; i < n && !r; i++) {
            char c[NBFS_PATH_MAX];
            nbfs_path_join(path, items[i].name, c, sizeof c);
            r = vfs_remove_tree(c);
        }
        if (n > 0) kfree(items);
        if (r) return r;
    }
    return fat32_remove(usb_sub(path));
}

int vfs_copy_tree(const char *src, const char *dst)
{
    if (!vfs_exists(src)) return NBFS_ENOENT;
    int r;
    if (!vfs_is_dir(src)) {
        char *data;
        u32 len;
        r = vfs_read_all(src, &data, &len);
        if (r) return r;
        r = vfs_write_all(dst, data, len);
        kfree(data);
        return r;
    }
    r = vfs_mkdir(dst);
    if (r) return r;
    struct vfs_item *items;
    int n = vfs_list(src, &items);
    for (int i = 0; i < n && !r; i++) {
        char a[NBFS_PATH_MAX], b[NBFS_PATH_MAX];
        nbfs_path_join(src, items[i].name, a, sizeof a);
        nbfs_path_join(dst, items[i].name, b, sizeof b);
        r = vfs_copy_tree(a, b);
    }
    if (n > 0) kfree(items);
    return r;
}

int vfs_move_into(const char *src, const char *dir, bool copy, char *out, int max)
{
    char parent[NBFS_PATH_MAX], dst[NBFS_PATH_MAX];
    const char *name = nbfs_path_basename(src);
    nbfs_path_parent(src, parent, sizeof parent);
    if (!vfs_is_dir(dir)) return NBFS_ENOTDIR;
    nbfs_path_join(dir, name, dst, sizeof dst);
    if (!copy && !strcmp(parent, dir)) { strlcpy(out, src, max); return NBFS_OK; }   /* nothing to do */
    /* never into itself */
    int sl = strlen(src);
    if (!strncmp(dir, src, sl) && (dir[sl] == '/' || dir[sl] == 0)) return NBFS_EINVAL;
    if (vfs_exists(dst)) {
        if (!copy) return NBFS_EEXIST;
        const char *dot = strrchr(name, '.');
        int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
        for (int i = 2; i < 100 && vfs_exists(dst); i++) {
            char nn[NBFS_NAME_MAX + 1];
            snprintf(nn, sizeof nn, "%.*s (%d)%s", base, name, i, dot && dot != name ? dot : "");
            nbfs_path_join(dir, nn, dst, sizeof dst);
        }
    }
    int r;
    if (copy) r = vfs_copy_tree(src, dst);
    else if (vfs_is_usb(src) != vfs_is_usb(dst)) {          /* moving between the system disk and the USB disk: copy, then delete */
        r = vfs_copy_tree(src, dst);
        if (!r) r = vfs_remove_tree(src);
    } else r = vfs_rename(src, dst);
    if (!r) strlcpy(out, dst, max);
    return r;
}
