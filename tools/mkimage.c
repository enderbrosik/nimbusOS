/*
 * NimbusOS - host tool: assemble a bootable raw disk image (.img file).
 *
 *   mkimage <out.img> <size-MiB> <mbr.bin> <stage2.bin> <kernel.bin>
 *
 * Disk layout (512-byte sectors):
 *   LBA 0          MBR (stage 1) + boot info block + partition table
 *   LBA 1..63      stage 2 (max 31.5 KiB)
 *   LBA 64..4095   kernel (max ~2 MiB)
 *   LBA 4096..end  NimbusFS partition (type 0x4E), formatted by nfstool
 *
 * This tool only ever creates/overwrites the regular file named on the
 * command line; it refuses to touch block/character devices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SECTOR 512
#define STAGE2_LBA 1
#define KERNEL_LBA 64
#define FS_LBA 4096
#define PART_TYPE 0x4E

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len + SECTOR);
    memset(b, 0, *len + SECTOR);
    if (fread(b, 1, *len, f) != *len) { perror(path); exit(1); }
    fclose(f);
    return b;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s out.img size-MiB mbr.bin stage2.bin kernel.bin\n", argv[0]);
        return 1;
    }
    const char *out = argv[1];
    uint32_t mib = atoi(argv[2]);
    struct stat st;
    if (stat(out, &st) == 0 && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "mkimage: refusing to write to non-regular file %s\n", out);
        return 1;
    }
    if (mib < 4) { fprintf(stderr, "mkimage: image must be >= 4 MiB\n"); return 1; }
    uint32_t total = mib * 2048;

    size_t mbr_len, s2_len, k_len;
    uint8_t *mbr = read_file(argv[3], &mbr_len);
    uint8_t *s2 = read_file(argv[4], &s2_len);
    uint8_t *k = read_file(argv[5], &k_len);
    if (mbr_len != SECTOR) { fprintf(stderr, "mbr must be 512 bytes\n"); return 1; }
    uint32_t s2_sec = (s2_len + SECTOR - 1) / SECTOR;
    uint32_t k_sec = (k_len + SECTOR - 1) / SECTOR;
    if (STAGE2_LBA + s2_sec > KERNEL_LBA) { fprintf(stderr, "stage2 too big\n"); return 1; }
    if (KERNEL_LBA + k_sec > FS_LBA) { fprintf(stderr, "kernel too big (%u sectors)\n", k_sec); return 1; }

    if (memcmp(mbr + 0x1A0, "NBT1", 4) != 0) { fprintf(stderr, "mbr: boot info magic missing\n"); return 1; }
    put32(mbr + 0x1A4, STAGE2_LBA);
    put16(mbr + 0x1A8, s2_sec);
    put32(mbr + 0x1AC, KERNEL_LBA);
    put32(mbr + 0x1B0, k_sec);
    put32(mbr + 0x1B8, (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ 0x4E494D42);

    /* Partition entry 0: bootable, LBA addressed, type 0x4E (NimbusFS).
     * CHS fields are set to the "use LBA" marker 0xFE/0xFF/0xFF. */
    uint8_t *pe = mbr + 0x1BE;
    memset(mbr + 0x1BE, 0, 64);
    pe[0] = 0x80;
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;
    pe[4] = PART_TYPE;
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    put32(pe + 8, FS_LBA);
    put32(pe + 12, total - FS_LBA);

    int fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(out); return 1; }
    if (ftruncate(fd, (off_t)total * SECTOR) != 0) { perror("ftruncate"); return 1; }
    if (pwrite(fd, mbr, SECTOR, 0) != SECTOR ||
        pwrite(fd, s2, s2_sec * SECTOR, (off_t)STAGE2_LBA * SECTOR) != (ssize_t)(s2_sec * SECTOR) ||
        pwrite(fd, k, k_sec * SECTOR, (off_t)KERNEL_LBA * SECTOR) != (ssize_t)(k_sec * SECTOR)) {
        perror("write");
        return 1;
    }
    close(fd);
    printf("mkimage: %s %u MiB, stage2 %u sectors, kernel %u sectors (%zu bytes), fs at LBA %u\n",
           out, mib, s2_sec, k_sec, k_len, FS_LBA);
    return 0;
}
