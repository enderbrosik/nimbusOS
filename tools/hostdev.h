/* NimbusOS host tools - file-backed block device for fs/nbfs.c.
 * Only regular files are accepted; block/character devices are refused. */
#ifndef HOSTDEV_H
#define HOSTDEV_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "fs/nbfs.h"

#ifdef _WIN32          /* Windows (MinGW): no pread/pwrite, and files must be opened in binary mode */
#include <io.h>
static ssize_t pread(int fd, void *buf, size_t n, uint64_t off)
{
    if (_lseeki64(fd, (long long)off, SEEK_SET) < 0) return -1;
    return read(fd, buf, (unsigned)n);
}
static ssize_t pwrite(int fd, const void *buf, size_t n, uint64_t off)
{
    if (_lseeki64(fd, (long long)off, SEEK_SET) < 0) return -1;
    return write(fd, buf, (unsigned)n);
}
#define HD_OPEN_FLAGS (O_RDWR | O_BINARY)
#else
#define HD_OPEN_FLAGS O_RDWR
#endif

struct hostdev { int fd; uint64_t offset; };

static int hd_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    struct hostdev *h = ctx;
    ssize_t n = pread(h->fd, buf, (size_t)count * 512, h->offset + (uint64_t)lba * 512);
    if (n < 0) return -1;
    if ((size_t)n < (size_t)count * 512) memset((char *)buf + n, 0, (size_t)count * 512 - n);
    return 0;
}

static int hd_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    struct hostdev *h = ctx;
    return pwrite(h->fd, buf, (size_t)count * 512, h->offset + (uint64_t)lba * 512) == (ssize_t)count * 512 ? 0 : -1;
}

/* Opens an image file.  If it starts with an MBR whose first partition has
 * type 0x4E (NimbusFS), the device covers that partition; otherwise the
 * whole file is the file system. */
static int hostdev_open(const char *path, struct hostdev *h, struct nbfs_blkdev *dev, int quiet)
{
    struct stat st;
    if (stat(path, &st) != 0) { perror(path); return -1; }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s: refusing to operate on a non-regular file (only .img files are allowed)\n", path);
        return -1;
    }
    h->fd = open(path, HD_OPEN_FLAGS);
    if (h->fd < 0) { perror(path); return -1; }
    h->offset = 0;
    uint64_t sectors = st.st_size / 512;
    unsigned char mbr[512];
    if (pread(h->fd, mbr, 512, 0) == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA && mbr[0x1BE + 4] == 0x4E) {
        uint32_t lba, cnt;
        memcpy(&lba, mbr + 0x1BE + 8, 4);
        memcpy(&cnt, mbr + 0x1BE + 12, 4);
        h->offset = (uint64_t)lba * 512;
        sectors = cnt;
        if (!quiet) fprintf(stderr, "(using NimbusFS partition at LBA %u, %u sectors)\n", lba, cnt);
    }
    dev->ctx = h;
    dev->read = hd_read;
    dev->write = hd_write;
    dev->sectors = (uint32_t)sectors;
    return 0;
}
#endif
