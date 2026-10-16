/*
 * NimbusOS - mkiso: wrap a bootable disk image in an ISO 9660 CD image with
 * an El Torito boot entry using HARD-DISK EMULATION.
 *
 *   mkiso <out.iso> <disk.img>
 *
 * The BIOS presents the embedded disk image as drive 0x80 and boots its MBR,
 * so the normal NimbusOS boot loader runs unchanged.  The ISO also exposes
 * the image as the file NIMBUS.IMG.
 *
 * Layout (2048-byte sectors):
 *   0-15  system area (zero)      19  L path table     22  boot catalog
 *   16    primary volume desc.    20  M path table     23+ disk image
 *   17    boot record (El Torito) 21  root directory
 *   18    descriptor terminator
 *
 * References: ECMA-119 (ISO 9660) sections 8.4, 9.1, 9.4; "El Torito"
 * Bootable CD-ROM Format Specification 1.0, sections 2.0-2.3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define SECT 2048
enum { S_PVD = 16, S_BOOTREC, S_TERM, S_LPATH, S_MPATH, S_ROOT, S_CATALOG, S_IMAGE };

static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void both16(uint8_t *p, uint16_t v) { le16(p, v); be16(p + 2, v); }
static void both32(uint8_t *p, uint32_t v) { le32(p, v); be32(p + 4, v); }
static void strpad(uint8_t *p, const char *s, int n) { memset(p, ' ', n); memcpy(p, s, strlen(s) < (size_t)n ? strlen(s) : (size_t)n); }

static struct tm now_tm;

/* 7-byte directory record date */
static void dir_date(uint8_t *p)
{
    p[0] = now_tm.tm_year; p[1] = now_tm.tm_mon + 1; p[2] = now_tm.tm_mday;
    p[3] = now_tm.tm_hour; p[4] = now_tm.tm_min; p[5] = now_tm.tm_sec; p[6] = 0;
}

/* 17-byte volume descriptor date */
static void vd_date(uint8_t *p)
{
    char b[64];
    snprintf(b, sizeof b, "%04d%02d%02d%02d%02d%02d00", now_tm.tm_year + 1900, now_tm.tm_mon + 1,
             now_tm.tm_mday, now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec);
    memcpy(p, b, 16);
    p[16] = 0;
}

static int dirrec(uint8_t *p, uint32_t extent, uint32_t size, int is_dir, const char *name, int namelen)
{
    int len = 33 + namelen + ((33 + namelen) & 1);
    memset(p, 0, len);
    p[0] = len;
    both32(p + 2, extent);
    both32(p + 10, size);
    dir_date(p + 18);
    p[25] = is_dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = namelen;
    memcpy(p + 33, name, namelen);
    return len;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: mkiso <out.iso> <disk.img>\n"); return 1; }
    struct stat st;
    if (stat(argv[1], &st) == 0 && !S_ISREG(st.st_mode)) { fprintf(stderr, "mkiso: refusing to write to %s\n", argv[1]); return 1; }
    FILE *in = fopen(argv[2], "rb");
    if (!in) { perror(argv[2]); return 1; }
    fseek(in, 0, SEEK_END);
    long img_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (img_size <= 0 || img_size % 512) { fprintf(stderr, "mkiso: bad image size\n"); return 1; }
    uint8_t mbr[512];
    if (fread(mbr, 1, 512, in) != 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) { fprintf(stderr, "mkiso: image has no MBR\n"); return 1; }
    fseek(in, 0, SEEK_SET);

    time_t t = time(NULL);
    now_tm = *gmtime(&t);
    uint32_t img_sectors = (img_size + SECT - 1) / SECT;
    uint32_t total = S_IMAGE + img_sectors;
    uint8_t *iso = calloc(S_IMAGE, SECT);

    /* Primary volume descriptor */
    uint8_t *pvd = iso + S_PVD * SECT;
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    strpad(pvd + 8, "NIMBUSOS", 32);
    strpad(pvd + 40, "NIMBUSOS_LIVE", 32);
    both32(pvd + 80, total);
    both16(pvd + 120, 1);
    both16(pvd + 124, 1);
    both16(pvd + 128, SECT);
    both32(pvd + 132, 10);                 /* path table size */
    le32(pvd + 140, S_LPATH);
    be32(pvd + 148, S_MPATH);
    dirrec(pvd + 156, S_ROOT, SECT, 1, "\0", 1);
    strpad(pvd + 190, "", 128);
    strpad(pvd + 318, "NIMBUSOS", 128);
    strpad(pvd + 446, "NIMBUSOS MKISO", 128);
    strpad(pvd + 574, "NIMBUSOS", 128);
    strpad(pvd + 702, "", 37); strpad(pvd + 739, "", 37); strpad(pvd + 776, "", 37);
    vd_date(pvd + 813); vd_date(pvd + 830);
    memcpy(pvd + 847, "0000000000000000", 16); memcpy(pvd + 864, "0000000000000000", 16);
    pvd[881] = 1;

    /* El Torito boot record */
    uint8_t *br = iso + S_BOOTREC * SECT;
    br[0] = 0; memcpy(br + 1, "CD001", 5); br[6] = 1;
    memcpy(br + 7, "EL TORITO SPECIFICATION", 23);
    le32(br + 71, S_CATALOG);

    /* Terminator */
    uint8_t *term = iso + S_TERM * SECT;
    term[0] = 255; memcpy(term + 1, "CD001", 5); term[6] = 1;

    /* Path tables: just the root directory */
    uint8_t *lp = iso + S_LPATH * SECT, *mp = iso + S_MPATH * SECT;
    lp[0] = 1; le32(lp + 2, S_ROOT); le16(lp + 6, 1);
    mp[0] = 1; be32(mp + 2, S_ROOT); be16(mp + 6, 1);

    /* Root directory: ".", "..", BOOT.CAT, NIMBUS.IMG */
    uint8_t *rd = iso + S_ROOT * SECT;
    int off = 0;
    off += dirrec(rd + off, S_ROOT, SECT, 1, "\0", 1);
    off += dirrec(rd + off, S_ROOT, SECT, 1, "\1", 1);
    off += dirrec(rd + off, S_CATALOG, SECT, 0, "BOOT.CAT;1", 10);
    off += dirrec(rd + off, S_IMAGE, (uint32_t)img_size, 0, "NIMBUS.IMG;1", 12);

    /* Boot catalog: validation entry + initial/default entry */
    uint8_t *cat = iso + S_CATALOG * SECT;
    cat[0] = 1;                            /* header id */
    cat[1] = 0;                            /* platform: 80x86 */
    memcpy(cat + 4, "NimbusOS", 8);
    cat[30] = 0x55; cat[31] = 0xAA;
    uint16_t sum = 0;
    for (int i = 0; i < 32; i += 2) sum += cat[i] | (cat[i + 1] << 8);
    le16(cat + 28, (uint16_t)(0x10000 - sum));
    uint8_t *ent = cat + 32;
    ent[0] = 0x88;                         /* bootable */
    ent[1] = 4;                            /* media: hard disk emulation */
    le16(ent + 2, 0);                      /* load segment: default 0x7C0 */
    ent[4] = mbr[0x1BE + 4];               /* system type = partition type */
    le16(ent + 6, 1);                      /* sectors to load: the MBR */
    le32(ent + 8, S_IMAGE);

    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 1; }
    fwrite(iso, SECT, S_IMAGE, out);
    uint8_t buf[65536];
    size_t n, written = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) { fwrite(buf, 1, n, out); written += n; }
    static uint8_t zero[SECT];
    if (written % SECT) fwrite(zero, 1, SECT - written % SECT, out);
    fclose(out);
    fclose(in);
    printf("mkiso: %s: %u sectors (%u MiB), El Torito HD emulation, image at sector %d\n",
           argv[1], total, total / 512, S_IMAGE);
    return 0;
}
