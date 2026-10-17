/*
 * NimbusOS - FAT32 driver.  Sector-at-a-time, no cache beyond one FAT sector, so it is
 * slow but simple and always consistent on disk (every change is written at once).
 */
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "drivers/ata.h"
#include "drivers/rtc.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define E_NOENT (-2)
#define E_IO (-5)
#define E_EXIST (-17)
#define E_NOTDIR (-20)
#define E_ISDIR (-21)
#define E_INVAL (-22)
#define E_NOSPC (-28)
#define E_NAMELEN (-36)
#define E_NOTEMPTY (-39)
#define E_BADFS (-77)

#define ATTR_RO 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F
#define FAT_EOC 0x0FFFFFF8u
#define MAX_FILE (256u * 1024 * 1024)

static struct {
    bool mounted;
    int drive;
    u32 part_lba;
    u32 spc, reserved, nfats, fatsz, rootclus, nclusters, data_lba, total_sectors;
    u32 next_free, free_count, fsinfo_sec;
    char label[12];
    u8 fatbuf[512];
    u32 fatbuf_sec;                 /* FAT sector held in fatbuf, or 0xFFFFFFFF */
} V;

static int rd(u32 lba, void *buf) { return ata_read(V.drive, V.part_lba + lba, 1, buf) ? E_IO : 0; }
static int wr(u32 lba, const void *buf) { return ata_write(V.drive, V.part_lba + lba, 1, buf) ? E_IO : 0; }
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8 *p) { return p[0] | p[1] << 8; }
static void put16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static u32 clus_lba(u32 c) { return V.data_lba + (c - 2) * V.spc; }
static u32 clus_bytes(void) { return V.spc * 512; }

/* ------------------------------------------------------------------ FAT table */
static int fat_load(u32 sec)
{
    if (V.fatbuf_sec == sec) return 0;
    int r = rd(V.reserved + sec, V.fatbuf);
    if (r) return r;
    V.fatbuf_sec = sec;
    return 0;
}

static int fat_get(u32 c, u32 *v)
{
    int r = fat_load(c * 4 / 512);
    if (r) return r;
    *v = le32(V.fatbuf + c * 4 % 512) & 0x0FFFFFFF;
    return 0;
}

static int fat_set(u32 c, u32 v)
{
    int r = fat_load(c * 4 / 512);
    if (r) return r;
    u8 *p = V.fatbuf + c * 4 % 512;
    put32(p, (le32(p) & 0xF0000000) | (v & 0x0FFFFFFF));
    for (u32 f = 0; f < V.nfats; f++)
        if ((r = wr(V.reserved + f * V.fatsz + c * 4 / 512, V.fatbuf))) return r;
    return 0;
}

static int clus_zero(u32 c)
{
    u8 z[512];
    memset(z, 0, sizeof z);
    for (u32 i = 0; i < V.spc; i++) { int r = wr(clus_lba(c) + i, z); if (r) return r; }
    return 0;
}

static int alloc_clus(u32 *out, bool zero)
{
    for (u32 n = 0; n < V.nclusters; n++) {
        u32 c = 2 + (V.next_free - 2 + n) % V.nclusters, v;
        int r = fat_get(c, &v);
        if (r) return r;
        if (v) continue;
        if ((r = fat_set(c, 0x0FFFFFFF))) return r;
        V.free_count--;
        if (zero && (r = clus_zero(c))) return r;
        V.next_free = c + 1 >= V.nclusters + 2 ? 2 : c + 1;
        *out = c;
        return 0;
    }
    return E_NOSPC;
}

static int free_chain(u32 c)
{
    while (c >= 2 && c < FAT_EOC) {
        u32 nx;
        int r = fat_get(c, &nx);
        if (r || (r = fat_set(c, 0))) return r;
        V.free_count++;
        c = nx;
    }
    return 0;
}

/* keeps the free-cluster count in the FSInfo sector right, so other systems see a clean volume */
static void fsinfo_flush(void)
{
    u8 b[512];
    if (!V.fsinfo_sec || rd(V.fsinfo_sec, b)) return;
    put32(b + 488, V.free_count);
    put32(b + 492, V.next_free);
    wr(V.fsinfo_sec, b);
}

static u32 count_free(void)
{
    u32 n = 0, v;
    for (u32 c = 2; c < V.nclusters + 2; c++) if (!fat_get(c, &v) && !v) n++;
    return n;
}

/* ------------------------------------------------------------------ directories */
struct dent {
    char name[FAT_NAME_MAX + 1];
    u8 sfn[11], attr;
    u32 cluster, size;
    u32 first, idx;                 /* entry indexes: first LFN piece (or the entry itself) .. the short entry */
    bool lfn;
};

/* Entry n of the directory that starts at cluster dir.  With grow set, a missing
 * cluster is added.  Fills lba and the offset inside that sector. */
static int dir_locate(u32 dir, u32 n, bool grow, u32 *lba, u32 *off)
{
    u32 per = clus_bytes() / 32, c = dir, steps = n / per;
    while (steps--) {
        u32 nx;
        int r = fat_get(c, &nx);
        if (r) return r;
        if (nx < 2 || nx >= FAT_EOC) {
            if (!grow) return E_NOENT;
            if ((r = alloc_clus(&nx, true)) || (r = fat_set(c, nx))) return r;
        }
        c = nx;
    }
    u32 in = n % per * 32;
    *lba = clus_lba(c) + in / 512;
    *off = in % 512;
    return 0;
}

static int dir_get(u32 dir, u32 n, u8 *e)
{
    u32 lba, off;
    u8 sec[512];
    int r = dir_locate(dir, n, false, &lba, &off);
    if (r || (r = rd(lba, sec))) return r;
    memcpy(e, sec + off, 32);
    return 0;
}

static int dir_put(u32 dir, u32 n, const u8 *e)
{
    u32 lba, off;
    u8 sec[512];
    int r = dir_locate(dir, n, true, &lba, &off);
    if (r || (r = rd(lba, sec))) return r;
    memcpy(sec + off, e, 32);
    return wr(lba, sec);
}

static u8 sfn_sum(const u8 *s) { u8 sum = 0; for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + s[i]); return sum; }

static void sfn_to_name(const u8 *s, u8 flags, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && s[i] != ' '; i++) { char ch = s[i]; if ((flags & 0x08) && ch >= 'A' && ch <= 'Z') ch += 32; out[n++] = ch; }
    if (s[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && s[i] != ' '; i++) { char ch = s[i]; if ((flags & 0x10) && ch >= 'A' && ch <= 'Z') ch += 32; out[n++] = ch; }
    }
    out[n] = 0;
}

/* Calls cb for every file or folder of dir (not "." and ".."), until cb returns nonzero. */
static int dir_scan(u32 dir, int (*cb)(void *, struct dent *), void *ctx)
{
    struct dent d;
    char lname[FAT_NAME_MAX + 1];
    u8 e[32];
    int lsum = -1;
    u32 lfirst = 0;
    bool lvalid = false;
    for (u32 n = 0;; n++) {
        int r = dir_get(dir, n, e);
        if (r == E_NOENT) return 0;
        if (r) return r;
        if (e[0] == 0) return 0;
        if (e[0] == 0xE5) { lvalid = false; continue; }
        if (e[11] == ATTR_LFN) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { memset(lname, 0, sizeof lname); lsum = e[13]; lfirst = n; lvalid = true; }
            if (!lvalid || seq < 1 || seq > 20 || e[13] != lsum) { lvalid = false; continue; }
            static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < 13; i++) {
                u32 ch = le16(e + pos[i]);
                int at = (seq - 1) * 13 + i;
                if (ch == 0 || ch == 0xFFFF || at >= FAT_NAME_MAX) continue;
                lname[at] = ch < 128 ? (char)ch : '?';
            }
            continue;
        }
        if (e[11] & ATTR_VOLUME) { lvalid = false; continue; }
        memset(&d, 0, sizeof d);
        memcpy(d.sfn, e, 11);
        d.attr = e[11];
        d.cluster = le16(e + 20) << 16 | le16(e + 26);
        d.size = le32(e + 28);
        d.idx = n;
        d.first = n;
        if (lvalid && lsum == sfn_sum(e) && lname[0]) { strlcpy(d.name, lname, sizeof d.name); d.first = lfirst; d.lfn = true; }
        else sfn_to_name(e, e[12], d.name);
        lvalid = false;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        if ((r = cb(ctx, &d))) return r < 0 ? r : 0;
    }
}

struct find { const char *name; struct dent *out; bool found; const u8 *sfn; };
static int find_cb(void *ctx, struct dent *d)
{
    struct find *f = ctx;
    if (f->sfn ? !memcmp(d->sfn, f->sfn, 11) : !strcasecmp(d->name, f->name)) { *f->out = *d; f->found = true; return 1; }
    return 0;
}

/* The folder that holds the last component of path.  Fills the last name; dir is its cluster. */
static int walk(const char *path, u32 *dir, char *last)
{
    u32 cur = V.rootclus;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) { *dir = cur; last[0] = 0; return 0; }
    for (;;) {
        char comp[FAT_NAME_MAX + 1];
        int n = 0;
        while (*p && *p != '/') { if (n >= FAT_NAME_MAX) return E_NAMELEN; comp[n++] = *p++; }
        comp[n] = 0;
        while (*p == '/') p++;
        if (!*p) { strlcpy(last, comp, FAT_NAME_MAX + 1); *dir = cur; return 0; }
        struct dent d;
        struct find f = { comp, &d, false, NULL };
        int r = dir_scan(cur, find_cb, &f);
        if (r) return r;
        if (!f.found) return E_NOENT;
        if (!(d.attr & ATTR_DIR)) return E_NOTDIR;
        cur = d.cluster ? d.cluster : V.rootclus;
    }
}

static int lookup(const char *path, struct dent *d, u32 *dir)
{
    char last[FAT_NAME_MAX + 1];
    int r = walk(path, dir, last);
    if (r) return r;
    if (!last[0]) {                                   /* the root folder */
        memset(d, 0, sizeof *d);
        d->attr = ATTR_DIR;
        d->cluster = V.rootclus;
        return 0;
    }
    struct find f = { last, d, false, NULL };
    if ((r = dir_scan(*dir, find_cb, &f))) return r;
    return f.found ? 0 : E_NOENT;
}

/* ------------------------------------------------------------------ creating entries */
static void stamp(u16 *date, u16 *time)
{
    struct rtc_time t;
    rtc_read(&t);
    int y = t.year < 1980 ? 1980 : t.year;
    *date = (u16)(((y - 1980) << 9) | (t.month << 5) | t.day);
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

static bool sfn_char_ok(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c) != NULL; }

/* Makes the 11-byte short name for name.  Returns true when the name fits 8.3 exactly (no long name needed). */
static bool make_sfn(const char *name, u8 *sfn, int tail)
{
    memset(sfn, ' ', 11);
    int len = strlen(name), dot = -1;
    for (int i = len - 1; i >= 0; i--) if (name[i] == '.') { dot = i; break; }
    bool exact = true;
    int bn = 0, en = 0;
    for (int i = 0; i < len; i++) {
        char c = name[i];
        if (i == dot) continue;
        bool in_ext = dot >= 0 && i > dot;
        if (c == ' ' || (c == '.' && !in_ext)) { exact = false; continue; }
        if (c >= 'a' && c <= 'z') { c -= 32; exact = false; }
        if (!sfn_char_ok(c)) { c = '_'; exact = false; }
        if (in_ext) { if (en < 3) sfn[8 + en++] = c; else exact = false; }
        else { if (bn < 8) sfn[bn++] = c; else exact = false; }
    }
    if (dot == 0 || bn == 0) exact = false;
    if (tail) {                                        /* NAME~1 */
        char t[8];
        int tl = snprintf(t, sizeof t, "~%d", tail);
        int at = bn > 8 - tl ? 8 - tl : bn;
        for (int i = 0; i < tl; i++) sfn[at + i] = t[i];
    }
    return exact;
}

static void fill_lfn(u8 *e, const char *name, int len, int seq, bool last, u8 sum)
{
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    memset(e, 0, 32);
    e[0] = seq | (last ? 0x40 : 0);
    e[11] = ATTR_LFN;
    e[13] = sum;
    for (int i = 0; i < 13; i++) {
        int at = (seq - 1) * 13 + i;
        u32 ch = at < len ? (u8)name[at] : at == len ? 0 : 0xFFFF;
        put16(e + pos[i], ch);
    }
}

/* Adds a file or folder entry to dir.  Fills d (cluster and size as given). */
static int add_entry(u32 dir, const char *name, u8 attr, u32 cluster, u32 size)
{
    int len = strlen(name);
    if (!len || len > FAT_NAME_MAX) return E_NAMELEN;
    for (int i = 0; i < len; i++) if (strchr("\\:*?\"<>|", name[i]) || (u8)name[i] < 32) return E_INVAL;
    u8 sfn[11];
    bool exact = make_sfn(name, sfn, 0);
    if (!exact) {
        for (int tail = 1;; tail++) {
            if (tail > 9999) return E_EXIST;
            make_sfn(name, sfn, tail);
            struct dent tmp;
            struct find f = { NULL, &tmp, false, sfn };
            int r = dir_scan(dir, find_cb, &f);
            if (r) return r;
            if (!f.found) break;
        }
    }
    int pieces = exact ? 0 : (len + 12) / 13, need = pieces + 1;
    /* find a run of free slots */
    u32 n = 0, run = 0, start = 0;
    u8 e[32];
    for (;; n++) {
        int r = dir_get(dir, n, e);
        if (r == E_NOENT) { if (!run) start = n; break; }   /* past the last cluster: everything is free */
        if (r) return r;
        if (e[0] == 0) { if (!run) start = n; break; }       /* end marker: the rest is free */
        if (e[0] == 0xE5) { if (!run) start = n; if (++run >= (u32)need) break; }
        else run = 0;
    }
    u8 sum = sfn_sum(sfn);
    for (int i = 0; i < pieces; i++) {
        int seq = pieces - i;
        fill_lfn(e, name, len, seq, i == 0, sum);
        int r = dir_put(dir, start + i, e);
        if (r) return r;
    }
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    u16 date, time;
    stamp(&date, &time);
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date);
    put16(e + 22, time); put16(e + 24, date);
    put16(e + 20, cluster >> 16); put16(e + 26, cluster);
    put32(e + 28, size);
    return dir_put(dir, start + pieces, e);
}

static int update_entry(u32 dir, u32 idx, u32 cluster, u32 size)
{
    u8 e[32];
    int r = dir_get(dir, idx, e);
    if (r) return r;
    u16 date, time;
    stamp(&date, &time);
    put16(e + 20, cluster >> 16); put16(e + 26, cluster);
    put32(e + 28, size);
    put16(e + 22, time); put16(e + 24, date); put16(e + 18, date);
    e[11] |= 0x20;
    return dir_put(dir, idx, e);
}

/* ------------------------------------------------------------------ mounting */
static int try_volume(int drive, u32 lba)
{
    u8 b[512];
    if (ata_read(drive, lba, 1, b) || b[510] != 0x55 || b[511] != 0xAA) return E_BADFS;
    if (le16(b + 11) != 512 || !b[13] || le16(b + 22) != 0 || !le32(b + 36) || le16(b + 17) != 0) return E_BADFS;
    u32 total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    u32 spc = b[13], reserved = le16(b + 14), nfats = b[16], fatsz = le32(b + 36);
    if (!nfats || !reserved || total <= reserved + nfats * fatsz) return E_BADFS;
    u32 data = reserved + nfats * fatsz, ncl = (total - data) / spc;
    if (ncl < 65525) return E_BADFS;                  /* that is FAT12/16, not FAT32 */
    memset(&V, 0, sizeof V);
    V.drive = drive; V.part_lba = lba; V.spc = spc; V.reserved = reserved; V.nfats = nfats; V.fatsz = fatsz;
    V.rootclus = le32(b + 44); V.nclusters = ncl; V.data_lba = data; V.total_sectors = total;
    V.next_free = 2; V.fatbuf_sec = 0xFFFFFFFF;
    memcpy(V.label, b + 71, 11);
    for (int i = 10; i >= 0 && V.label[i] == ' '; i--) V.label[i] = 0;
    u32 fi = le16(b + 48);
    if (fi && fi < reserved) {
        u8 f[512];
        if (!rd(fi, f) && le32(f) == 0x41615252) V.fsinfo_sec = fi;
    }
    V.free_count = count_free();
    V.mounted = true;
    return 0;
}

int fat32_mount(void)
{
    if (V.mounted) return 0;
    for (int d = 0; d < ATA_MAX_DRIVES; d++) {
        if (!g_ata[d].present || d == boot_drive_index()) continue;
        u8 mbr[512];
        if (ata_read(d, 0, 1, mbr)) continue;
        if (!try_volume(d, 0)) return 0;                                 /* the whole disk is one volume */
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) continue;
        for (int p = 0; p < 4; p++) {
            const u8 *pe = mbr + 446 + p * 16;
            if ((pe[4] == 0x0B || pe[4] == 0x0C) && !try_volume(d, le32(pe + 8))) return 0;
        }
    }
    return E_NOENT;
}

bool fat32_mounted(void) { return V.mounted; }
const char *fat32_label(void) { return V.label; }
void fat32_info(int *drive, u32 *total_kib, u32 *free_kib)
{
    *drive = V.drive;
    *total_kib = V.nclusters * V.spc / 2;
    *free_kib = V.free_count * V.spc / 2;
}

/* ------------------------------------------------------------------ operations */
struct lst { struct fat_item *v; int n, cap; };
static int list_cb(void *ctx, struct dent *d)
{
    struct lst *l = ctx;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        struct fat_item *nv = kmalloc(nc * sizeof *nv);
        if (!nv) return E_NOSPC;
        if (l->v) { memcpy(nv, l->v, l->n * sizeof *nv); kfree(l->v); }
        l->v = nv; l->cap = nc;
    }
    struct fat_item *it = &l->v[l->n++];
    strlcpy(it->name, d->name, sizeof it->name);
    it->dir = d->attr & ATTR_DIR;
    it->size = d->size;
    return 0;
}

int fat32_list(const char *path, struct fat_item **out)
{
    if (!V.mounted) return E_NOENT;
    struct dent d; u32 dir;
    int r = lookup(path, &d, &dir);
    if (r) return r;
    if (!(d.attr & ATTR_DIR)) return E_NOTDIR;
    struct lst l = { NULL, 0, 0 };
    r = dir_scan(d.cluster ? d.cluster : V.rootclus, list_cb, &l);
    if (r) { kfree(l.v); return r; }
    *out = l.v;
    return l.n;
}

int fat32_read(const char *path, char **out, u32 *len)
{
    if (!V.mounted) return E_NOENT;
    struct dent d; u32 dir;
    int r = lookup(path, &d, &dir);
    if (r) return r;
    if (d.attr & ATTR_DIR) return E_ISDIR;
    if (d.size > MAX_FILE) return E_NOSPC;
    char *buf = kmalloc(d.size + 1);
    if (!buf) return E_NOSPC;
    u32 got = 0, c = d.cluster;
    u8 sec[512];
    while (got < d.size && c >= 2 && c < FAT_EOC) {
        for (u32 i = 0; i < V.spc && got < d.size; i++) {
            if ((r = rd(clus_lba(c) + i, sec))) { kfree(buf); return r; }
            u32 n = d.size - got < 512 ? d.size - got : 512;
            memcpy(buf + got, sec, n);
            got += n;
        }
        if ((r = fat_get(c, &c))) { kfree(buf); return r; }
    }
    if (got < d.size) { kfree(buf); return E_BADFS; }
    buf[d.size] = 0;
    *out = buf;
    *len = d.size;
    return 0;
}

static int fat32_write_do(const char *path, const void *data, u32 len)
{
    if (!V.mounted) return E_NOENT;
    if (len > MAX_FILE) return E_NOSPC;
    char last[FAT_NAME_MAX + 1];
    u32 dir;
    int r = walk(path, &dir, last);
    if (r) return r;
    if (!last[0]) return E_ISDIR;
    struct dent d;
    struct find f = { last, &d, false, NULL };
    if ((r = dir_scan(dir, find_cb, &f))) return r;
    if (f.found && (d.attr & ATTR_DIR)) return E_ISDIR;
    if (f.found && (d.attr & ATTR_RO)) return E_INVAL;
    if (f.found && (r = free_chain(d.cluster))) return r;
    /* allocate and write the new data first, then point the entry at it */
    u32 first = 0, prev = 0, done = 0;
    const u8 *src = data;
    u8 sec[512];
    while (done < len) {
        u32 c;
        if ((r = alloc_clus(&c, false))) { if (first) free_chain(first); if (f.found) update_entry(dir, d.idx, 0, 0); return r; }
        if (prev) r = fat_set(prev, c); else first = c;
        for (u32 i = 0; !r && i < V.spc; i++) {
            u32 n = len - done < 512 ? len - done : 512;
            if (n < 512) memset(sec, 0, sizeof sec);
            memcpy(sec, src + done, n);
            r = wr(clus_lba(c) + i, sec);
            done += n;
            if (done >= len) break;
        }
        if (r) { free_chain(first); return r; }
        prev = c;
    }
    if (f.found) return update_entry(dir, d.idx, first, len);
    return add_entry(dir, last, 0x20, first, len);
}

static int fat32_mkdir_do(const char *path)
{
    if (!V.mounted) return E_NOENT;
    char last[FAT_NAME_MAX + 1];
    u32 dir;
    int r = walk(path, &dir, last);
    if (r) return r;
    if (!last[0]) return E_EXIST;
    struct dent d;
    struct find f = { last, &d, false, NULL };
    if ((r = dir_scan(dir, find_cb, &f))) return r;
    if (f.found) return E_EXIST;
    u32 c;
    if ((r = alloc_clus(&c, true))) return r;
    u8 e[32];
    u16 date, time;
    stamp(&date, &time);
    for (int k = 0; k < 2; k++) {                      /* "." and ".." */
        memset(e, 0, 32);
        memset(e, ' ', 11);
        e[0] = '.'; if (k) e[1] = '.';
        e[11] = ATTR_DIR;
        u32 target = k ? (dir == V.rootclus ? 0 : dir) : c;
        put16(e + 14, time); put16(e + 16, date); put16(e + 18, date); put16(e + 22, time); put16(e + 24, date);
        put16(e + 20, target >> 16); put16(e + 26, target);
        if ((r = dir_put(c, k, e))) { free_chain(c); return r; }
    }
    if ((r = add_entry(dir, last, ATTR_DIR, c, 0))) free_chain(c);
    return r;
}

static int empty_cb(void *ctx, struct dent *d) { (void)d; *(bool *)ctx = false; return 1; }

static int fat32_remove_do(const char *path)
{
    if (!V.mounted) return E_NOENT;
    struct dent d; u32 dir;
    int r = lookup(path, &d, &dir);
    if (r) return r;
    if (d.idx == 0 && d.cluster == V.rootclus) return E_INVAL;
    if (d.attr & ATTR_DIR) {
        bool empty = true;
        if ((r = dir_scan(d.cluster, empty_cb, &empty))) return r;
        if (!empty) return E_NOTEMPTY;
    }
    u8 e[32];
    for (u32 n = d.first; n <= d.idx; n++) {            /* the long-name pieces and the short entry */
        if ((r = dir_get(dir, n, e))) return r;
        e[0] = 0xE5;
        if ((r = dir_put(dir, n, e))) return r;
    }
    return free_chain(d.cluster);
}

int fat32_write(const char *path, const void *data, u32 len)
{
    int r = fat32_write_do(path, data, len);
    fsinfo_flush();
    return r;
}

int fat32_mkdir(const char *path)
{
    int r = fat32_mkdir_do(path);
    fsinfo_flush();
    return r;
}

int fat32_remove(const char *path)
{
    int r = fat32_remove_do(path);
    fsinfo_flush();
    return r;
}
