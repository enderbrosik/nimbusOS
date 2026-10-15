/*
 * NimbusOS - nfstool: host utility for NimbusFS images.
 *
 *   nfstool mkfs   <new.img> <size-MiB> [label]   create a fresh, empty FS image
 *   nfstool format <image> [label]                 (re)format an image / its NimbusFS partition
 *   nfstool ls     <image> [path]
 *   nfstool tree   <image> [path]
 *   nfstool cat    <image> <path>
 *   nfstool put    <image> <host-file> <path>
 *   nfstool get    <image> <path> <host-file>
 *   nfstool mkdir  <image> <path>
 *   nfstool rm     <image> <path>                  (recursive)
 *   nfstool mv     <image> <from> <to>
 *   nfstool import <image> <host-dir> <path>       copy a host directory tree in
 *   nfstool check  <image>                         consistency check
 *   nfstool df     <image>
 *   nfstool blank  <new.img> <size-MiB>           create a blank (all-zero, sparse) disk image
 *
 * An image with an MBR whose first partition is type 0x4E is accessed
 * through that partition (that is how build/nimbus.img is laid out).
 */
#include "tools/hostdev.h"
#include <time.h>
#include <dirent.h>
#include <errno.h>

static nbfs_t fs;
static uint32_t host_now(void) { return (uint32_t)time(NULL); }

static void die(const char *what, int err)
{
    fprintf(stderr, "nfstool: %s: %s\n", what, nbfs_strerror(err));
    exit(1);
}

static void open_fs(const char *img)
{
    static struct hostdev h;
    struct nbfs_blkdev dev;
    if (hostdev_open(img, &h, &dev, 1) != 0) exit(1);
    fs.now = host_now;
    int r = nbfs_mount(&fs, &dev);
    if (r) die(img, r);
}

static void fmt_time(uint32_t t, char *out)
{
    time_t tt = t;
    struct tm *tm = gmtime(&tt);
    strftime(out, 32, "%Y-%m-%d %H:%M", tm);
}

static void do_ls(const char *path)
{
    struct nbfs_stat st;
    int r = nbfs_stat(&fs, path, &st);
    if (r) die(path, r);
    uint32_t cookie = 0;
    struct nbfs_entry e;
    while ((r = nbfs_readdir(&fs, st.inode, &cookie, &e)) == 1) {
        struct nbfs_stat cs;
        nbfs_stat_inode(&fs, e.inode, &cs);
        char t[32];
        fmt_time(cs.mtime, t);
        printf("%s %10u  %s  %s%s\n", e.type == NBFS_TYPE_DIR ? "d" : "-", cs.size, t, e.name,
               e.type == NBFS_TYPE_DIR ? "/" : "");
    }
    if (r < 0) die(path, r);
}

static void do_tree(const char *path, int depth)
{
    struct nbfs_stat st;
    if (nbfs_stat(&fs, path, &st)) return;
    uint32_t cookie = 0;
    struct nbfs_entry e;
    while (nbfs_readdir(&fs, st.inode, &cookie, &e) == 1) {
        struct nbfs_stat cs;
        nbfs_stat_inode(&fs, e.inode, &cs);
        printf("%*s%s%s", depth * 2, "", e.name, e.type == NBFS_TYPE_DIR ? "/\n" : "");
        if (e.type == NBFS_TYPE_DIR) {
            char child[NBFS_PATH_MAX];
            nbfs_path_join(path, e.name, child, sizeof child);
            do_tree(child, depth + 1);
        } else {
            printf("  (%u bytes)\n", cs.size);
        }
    }
}

static int put_file(const char *host, const char *path)
{
    FILE *f = fopen(host, "rb");
    if (!f) { perror(host); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) { perror(host); exit(1); }
    fclose(f);
    int r = nbfs_write_file(&fs, path, buf, len);
    free(buf);
    if (r < 0) die(path, r);
    return 0;
}

static void import_dir(const char *host, const char *path)
{
    DIR *d = opendir(host);
    if (!d) { perror(host); exit(1); }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;   /* skip ., .., and hidden files like .gitkeep */
        char hp[4096], gp[NBFS_PATH_MAX];
        snprintf(hp, sizeof hp, "%s/%s", host, de->d_name);
        nbfs_path_join(path, de->d_name, gp, sizeof gp);
        struct stat st;
        if (stat(hp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int r = nbfs_mkdir(&fs, gp);
            if (r && r != NBFS_EEXIST) die(gp, r);
            import_dir(hp, gp);
        } else if (S_ISREG(st.st_mode)) {
            put_file(hp, gp);
        }
    }
    closedir(d);
}

static void report(const char *msg) { fprintf(stderr, "  problem: %s\n", msg); }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: nfstool <mkfs|format|blank|ls|tree|cat|put|get|mkdir|rm|mv|import|check|df> <image> ...\n");
        return 1;
    }
    const char *cmd = argv[1], *img = argv[2];

    if (!strcmp(cmd, "mkfs")) {
        if (argc < 4) { fprintf(stderr, "usage: nfstool mkfs <new.img> <size-MiB> [label]\n"); return 1; }
        struct stat st;
        if (stat(img, &st) == 0 && !S_ISREG(st.st_mode)) { fprintf(stderr, "refusing to touch %s\n", img); return 1; }
        int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror(img); return 1; }
        long mib = atol(argv[3]);
        if (mib < 1 || mib > 4000) { fprintf(stderr, "size must be 1..4000 MiB\n"); return 1; }
        if (ftruncate(fd, (off_t)mib * 1024 * 1024) != 0) { perror("ftruncate"); return 1; }
        close(fd);
        struct hostdev h;
        struct nbfs_blkdev dev;
        if (hostdev_open(img, &h, &dev, 1)) return 1;
        int r = nbfs_format(&dev, argc > 4 ? argv[4] : "NIMBUS", host_now());
        if (r) die("format", r);
        printf("nfstool: created empty NimbusFS image %s (%ld MiB)\n", img, mib);
        return 0;
    }
    if (!strcmp(cmd, "blank")) {
        if (argc < 4) { fprintf(stderr, "usage: nfstool blank <new.img> <size-MiB>\n"); return 1; }
        struct stat st;
        if (stat(img, &st) == 0) { fprintf(stderr, "nfstool: %s already exists; delete it first\n", img); return 1; }
        int fd = open(img, O_RDWR | O_CREAT | O_EXCL, 0644);
        long mib = atol(argv[3]);
        if (fd < 0 || mib < 1 || ftruncate(fd, (off_t)mib * 1024 * 1024) != 0) { perror(img); return 1; }
        close(fd);
        printf("nfstool: created blank %ld MiB disk image %s\n", mib, img);
        return 0;
    }
    if (!strcmp(cmd, "format")) {
        struct hostdev h;
        struct nbfs_blkdev dev;
        if (hostdev_open(img, &h, &dev, 0)) return 1;
        int r = nbfs_format(&dev, argc > 3 ? argv[3] : "NIMBUS", host_now());
        if (r) die("format", r);
        printf("nfstool: formatted %s (%u KiB)\n", img, dev.sectors / 2);
        return 0;
    }

    open_fs(img);
    int r = 0;
    if (!strcmp(cmd, "ls")) do_ls(argc > 3 ? argv[3] : "/");
    else if (!strcmp(cmd, "tree")) { printf("/\n"); do_tree(argc > 3 ? argv[3] : "/", 1); }
    else if (!strcmp(cmd, "cat") && argc > 3) {
        struct nbfs_stat st;
        if ((r = nbfs_stat(&fs, argv[3], &st))) die(argv[3], r);
        char *buf = malloc(st.size + 1);
        r = nbfs_read(&fs, st.inode, 0, buf, st.size);
        if (r < 0) die(argv[3], r);
        fwrite(buf, 1, r, stdout);
        free(buf);
    } else if (!strcmp(cmd, "put") && argc > 4) put_file(argv[3], argv[4]);
    else if (!strcmp(cmd, "get") && argc > 4) {
        struct nbfs_stat st;
        if ((r = nbfs_stat(&fs, argv[3], &st))) die(argv[3], r);
        char *buf = malloc(st.size + 1);
        r = nbfs_read(&fs, st.inode, 0, buf, st.size);
        if (r < 0) die(argv[3], r);
        FILE *f = fopen(argv[4], "wb");
        if (!f) { perror(argv[4]); return 1; }
        fwrite(buf, 1, r, f);
        fclose(f);
    } else if (!strcmp(cmd, "mkdir") && argc > 3) { if ((r = nbfs_mkdir(&fs, argv[3]))) die(argv[3], r); }
    else if (!strcmp(cmd, "rm") && argc > 3) { if ((r = nbfs_remove_tree(&fs, argv[3]))) die(argv[3], r); }
    else if (!strcmp(cmd, "mv") && argc > 4) { if ((r = nbfs_rename(&fs, argv[3], argv[4]))) die(argv[3], r); }
    else if (!strcmp(cmd, "import") && argc > 4) {
        if (strcmp(argv[4], "/")) { r = nbfs_mkdir(&fs, argv[4]); if (r && r != NBFS_EEXIST) die(argv[4], r); }
        import_dir(argv[3], argv[4]);
    } else if (!strcmp(cmd, "check")) {
        int p = nbfs_check(&fs, report);
        printf("nfstool: %s: %d problem(s)\n", img, p);
        return p ? 2 : 0;
    } else if (!strcmp(cmd, "df")) {
        uint32_t total, freeb;
        nbfs_statfs(&fs, &total, &freeb);
        printf("label %.32s: %u KiB data, %u KiB free, %u/%u inodes free\n", fs.sb.label, total / 1024,
               freeb / 1024, fs.sb.free_inodes, fs.sb.inode_count);
    } else {
        fprintf(stderr, "nfstool: bad command or arguments\n");
        return 1;
    }
    return 0;
}
