/*
 * NimbusOS - newfs_nimbusfs_fmt: the "format" helper that Disk Utility / diskutil run from
 * /Library/Filesystems/nimbusfs_fmt.fs when you Erase a disk as NimbusFS.
 *
 *   newfs_nimbusfs_fmt [-V|-v] [name] <device-or-image>
 *
 * It formats the given block device (or image file) as an empty NimbusFS volume. It is called by the
 * system with the partition it just created, so it writes only to the path it is given. It refuses
 * anything above 4000 MiB because NimbusFS is limited to that size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/disk.h>
#include <sys/ioctl.h>
#endif
#include "fs/nbfs.h"

static int dev_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    ssize_t n = pread(*(int *)ctx, buf, (size_t)count * 512, (off_t)lba * 512);
    return n == (ssize_t)count * 512 ? 0 : -1;
}

static int dev_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    return pwrite(*(int *)ctx, buf, (size_t)count * 512, (off_t)lba * 512) == (ssize_t)count * 512 ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *label = "NimbusFS", *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "-v") || !strcmp(argv[i], "-n")) {
            if (i + 1 < argc - 1) label = argv[++i];     /* the name, if one comes before the device */
        } else if (argv[i][0] != '-') {
            path = argv[i];                              /* the last non-option is the device */
        }
    }
    if (!path) { fprintf(stderr, "usage: newfs_nimbusfs_fmt [-V name] <device>\n"); return 1; }

    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "newfs_nimbusfs_fmt: %s: %s\n", path, strerror(errno)); return 1; }

    uint64_t sectors = 0;
#ifdef __APPLE__
    uint64_t blocks = 0; uint32_t bs = 0;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 && ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 && bs)
        sectors = blocks * bs / 512;
#endif
    if (!sectors) {
        struct stat st;
        if (fstat(fd, &st) != 0) { perror("fstat"); return 1; }
        sectors = (uint64_t)st.st_size / 512;
    }
    if (sectors < 2048) { fprintf(stderr, "newfs_nimbusfs_fmt: %s is too small\n", path); return 1; }
    if (sectors > 4000ull * 2048) { fprintf(stderr, "newfs_nimbusfs_fmt: %s is larger than 4000 MiB, which NimbusFS cannot use\n", path); return 1; }

    struct nbfs_blkdev dev = { .ctx = &fd, .read = dev_read, .write = dev_write, .sectors = (uint32_t)sectors };
    int r = nbfs_format(&dev, label, (uint32_t)time(NULL));
    if (r) { fprintf(stderr, "newfs_nimbusfs_fmt: format failed: %s\n", nbfs_strerror(r)); return 1; }
    fsync(fd);
    close(fd);
    printf("newfs_nimbusfs_fmt: formatted %s as NimbusFS \"%s\" (%llu sectors)\n", path, label, (unsigned long long)sectors);
    return 0;
}
