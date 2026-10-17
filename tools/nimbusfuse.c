/*
 * NimbusOS - nimbusfuse: mount a NimbusFS disk image as a folder on your computer, so you
 * can open it in Finder, Explorer or any program.  Uses the FUSE 2 API, which is provided by
 *   Linux:   libfuse 2        macOS: macFUSE        Windows: WinFsp (its FUSE compatibility layer)
 *
 *   nimbusfuse <image.img> <mount-point> [fuse options]      e.g.  nimbusfuse nimbus.img /Volumes/nimbus
 *
 * Only image files are accepted (the same rule as nfstool): this tool never opens a real disk.
 * NimbusFS is not safe for two writers, so the mount is single-threaded and every change is
 * written to the image at once.  Unmount (umount / fusermount -u / Ctrl+C) before booting the image.
 */
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <errno.h>
#include <time.h>
#include "tools/hostdev.h"

static nbfs_t fs;
static struct hostdev hd;

static int err(int r) { return r >= 0 ? 0 : r == NBFS_ENOENT ? -ENOENT : r == NBFS_EEXIST ? -EEXIST : r == NBFS_ENOTDIR ? -ENOTDIR :
                        r == NBFS_EISDIR ? -EISDIR : r == NBFS_ENOSPC ? -ENOSPC : r == NBFS_ENOTEMPTY ? -ENOTEMPTY :
                        r == NBFS_ENAMETOOLONG ? -ENAMETOOLONG : r == NBFS_EINVAL ? -EINVAL : -EIO; }
static uint32_t now(void) { return (uint32_t)time(NULL); }
static int done(int r) { if (r >= 0) nbfs_sync(&fs); return err(r); }

static int nf_getattr(const char *path, struct stat *sb)
{
    struct nbfs_stat st;
    int r = nbfs_stat(&fs, path, &st);
    if (r) return err(r);
    memset(sb, 0, sizeof *sb);
    sb->st_mode = st.type == NBFS_TYPE_DIR ? S_IFDIR | 0755 : S_IFREG | 0644;
    sb->st_nlink = st.type == NBFS_TYPE_DIR ? 2 : 1;
    sb->st_size = st.size;
    sb->st_mtime = st.mtime;
    sb->st_ctime = st.ctime;
    sb->st_atime = st.mtime;
    sb->st_uid = getuid();
    sb->st_gid = getgid();
    return 0;
}

static int nf_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off, struct fuse_file_info *fi)
{
    struct nbfs_stat st;
    int r = nbfs_stat(&fs, path, &st);
    if (r) return err(r);
    if (st.type != NBFS_TYPE_DIR) return -ENOTDIR;
    fill(buf, ".", NULL, 0);
    fill(buf, "..", NULL, 0);
    uint32_t cookie = 0;
    struct nbfs_entry e;
    while ((r = nbfs_readdir(&fs, st.inode, &cookie, &e)) == 1) fill(buf, e.name, NULL, 0);
    return err(r);
}

static int nf_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    struct nbfs_stat st;
    int r = nbfs_stat(&fs, path, &st);
    if (r) return err(r);
    if (st.type == NBFS_TYPE_DIR) return -EISDIR;
    if ((uint64_t)off >= st.size) return 0;
    if (size > (size_t)(st.size - off)) size = st.size - off;
    r = nbfs_read(&fs, st.inode, (uint32_t)off, buf, (uint32_t)size);
    return r < 0 ? err(r) : r;
}

static int nf_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    struct nbfs_stat st;
    int r = nbfs_stat(&fs, path, &st);
    if (r) return err(r);
    r = nbfs_write(&fs, st.inode, (uint32_t)off, buf, (uint32_t)size);
    if (r < 0) return err(r);
    nbfs_sync(&fs);
    return (int)size;
}

static int nf_create(const char *path, mode_t mode, struct fuse_file_info *fi) { return done(nbfs_create(&fs, path, NBFS_TYPE_FILE)); }
static int nf_mknod(const char *path, mode_t mode, dev_t dev) { return S_ISREG(mode) ? done(nbfs_create(&fs, path, NBFS_TYPE_FILE)) : -EPERM; }
static int nf_mkdir(const char *path, mode_t mode) { return done(nbfs_mkdir(&fs, path)); }
static int nf_unlink(const char *path) { return done(nbfs_unlink(&fs, path)); }
static int nf_rmdir(const char *path) { return done(nbfs_unlink(&fs, path)); }
static int nf_rename(const char *from, const char *to) { return done(nbfs_rename(&fs, from, to)); }
static int nf_open(const char *path, struct fuse_file_info *fi) { uint32_t ino; return err(nbfs_lookup(&fs, path, &ino)); }
static int nf_utimens(const char *path, const struct timespec ts[2]) { return 0; }
static int nf_chmod(const char *path, mode_t mode) { return 0; }
static int nf_chown(const char *path, uid_t u, gid_t g) { return 0; }

static int nf_truncate(const char *path, off_t size)
{
    struct nbfs_stat st;
    int r = nbfs_stat(&fs, path, &st);
    if (r) return err(r);
    return done(nbfs_truncate(&fs, st.inode, (uint32_t)size));
}

static int nf_statfs(const char *path, struct statvfs *sv)
{
    uint32_t total, freeb;
    nbfs_statfs(&fs, &total, &freeb);
    memset(sv, 0, sizeof *sv);
    sv->f_bsize = sv->f_frsize = 4096;
    sv->f_blocks = total / 4096;
    sv->f_bfree = sv->f_bavail = freeb / 4096;
    sv->f_namemax = NBFS_NAME_MAX;
    return 0;
}

static const struct fuse_operations ops = {
    .getattr = nf_getattr, .readdir = nf_readdir, .open = nf_open, .read = nf_read, .write = nf_write,
    .create = nf_create, .mknod = nf_mknod, .mkdir = nf_mkdir, .unlink = nf_unlink, .rmdir = nf_rmdir,
    .rename = nf_rename, .truncate = nf_truncate, .utimens = nf_utimens, .chmod = nf_chmod, .chown = nf_chown,
    .statfs = nf_statfs,
};

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: nimbusfuse <image.img> <mount-point> [fuse options]\n"); return 1; }
    struct nbfs_blkdev dev;
    if (hostdev_open(argv[1], &hd, &dev, 0) != 0) return 1;
    fs.now = now;
    int r = nbfs_mount(&fs, &dev);
    if (r) { fprintf(stderr, "nimbusfuse: %s: %s\n", argv[1], nbfs_strerror(r)); return 1; }
    /* FUSE gets the mount point and its options; "-s" keeps it single-threaded */
    char *fargv[64];
    int n = 0;
    fargv[n++] = argv[0];
    for (int i = 2; i < argc && n < 60; i++) fargv[n++] = argv[i];
    fargv[n++] = (char *)"-s";
    fargv[n++] = (char *)"-o";
    fargv[n++] = (char *)"fsname=NimbusFS";
    return fuse_main(n, fargv, &ops, NULL);
}
