/*
 * NimbusOS - host-side test suite for NimbusFS (fs/nbfs.c).
 * Run with `make test`.  Every test formats a fresh image file, exercises
 * the API, and finishes with the consistency checker (nbfs_check).
 */
#include "tools/hostdev.h"
#include <time.h>

static int failures, checks;
static const char *img_path = "build/test/fstest.img";
static nbfs_t fs;
static struct hostdev hd;
static struct nbfs_blkdev dev;
static uint32_t fake_time = 1700000000;
static uint32_t test_now(void) { return fake_time; }

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); checks++; if (_a != _b) { failures++; \
    fprintf(stderr, "  FAIL %s:%d: %s == %ld, expected %s == %ld\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

static void report(const char *m) { fprintf(stderr, "  fsck: %s\n", m); }

static void fresh(uint32_t mib)
{
    if (hd.fd > 0) close(hd.fd);
    int fd = open(img_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (ftruncate(fd, (off_t)mib * 1024 * 1024)) { perror("ftruncate"); exit(1); }
    close(fd);
    if (hostdev_open(img_path, &hd, &dev, 1)) exit(1);
    CHECK_EQ(nbfs_format(&dev, "TEST", fake_time), 0);
    fs.now = test_now;
    CHECK_EQ(nbfs_mount(&fs, &dev), 0);
}

static void remount(void)
{
    fs.now = test_now;
    CHECK_EQ(nbfs_mount(&fs, &dev), 0);
}

static void fsck(void) { CHECK_EQ(nbfs_check(&fs, report), 0); }

static void pattern(uint8_t *b, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 31 + seed * 7 + (i >> 9)) ^ seed);
}

static void test_format_mount(void)
{
    fresh(8);
    struct nbfs_stat st;
    CHECK_EQ(nbfs_stat(&fs, "/", &st), 0);
    CHECK_EQ(st.type, NBFS_TYPE_DIR);
    CHECK_EQ(st.size, 0);
    uint32_t total, freeb;
    nbfs_statfs(&fs, &total, &freeb);
    CHECK(total > 7 * 1024 * 1024 && freeb == total);
    /* garbage must be rejected */
    struct nbfs_blkdev bad = dev;
    bad.sectors = 10;
    nbfs_t tmp;
    memset(&tmp, 0, sizeof tmp);
    CHECK_EQ(nbfs_format(&bad, "X", 0), NBFS_EINVAL);
    fsck();
}

static void test_create_read_write(void)
{
    fresh(8);
    int ino = nbfs_create(&fs, "/hello.txt", NBFS_TYPE_FILE);
    CHECK(ino > 1);
    CHECK_EQ(nbfs_create(&fs, "/hello.txt", NBFS_TYPE_FILE), NBFS_EEXIST);
    const char *msg = "Hello, NimbusFS!";
    CHECK_EQ(nbfs_write(&fs, ino, 0, msg, strlen(msg)), (int)strlen(msg));
    char buf[64] = { 0 };
    CHECK_EQ(nbfs_read(&fs, ino, 0, buf, sizeof buf), (int)strlen(msg));
    CHECK(!strcmp(buf, msg));
    /* partial overwrite in the middle */
    CHECK_EQ(nbfs_write(&fs, ino, 7, "Nimbus", 6), 6);
    memset(buf, 0, sizeof buf);
    nbfs_read(&fs, ino, 0, buf, sizeof buf);
    CHECK(!strcmp(buf, "Hello, NimbusFS!"));
    /* read at offset, read past end */
    CHECK_EQ(nbfs_read(&fs, ino, 7, buf, 6), 6);
    CHECK(!memcmp(buf, "Nimbus", 6));
    CHECK_EQ(nbfs_read(&fs, ino, 100, buf, 6), 0);
    /* whole-file helpers */
    CHECK_EQ(nbfs_write_file(&fs, "/a.txt", "abc", 3), 3);
    CHECK_EQ(nbfs_write_file(&fs, "/a.txt", "x", 1), 1);       /* truncates */
    CHECK_EQ(nbfs_read_file(&fs, "/a.txt", buf, sizeof buf), 1);
    CHECK_EQ(nbfs_read_file(&fs, "/missing", buf, sizeof buf), NBFS_ENOENT);
    fsck();
}

static void test_large_files(void)
{
    fresh(32);
    /* sizes chosen to cover direct, single-indirect and double-indirect blocks */
    uint32_t sizes[] = { 1, 1023, 1024, 1025, 24 * 1024, 24 * 1024 + 1, 280 * 1024, 280 * 1024 + 5,
                         3 * 1024 * 1024 + 77 };
    uint32_t base_free = fs.sb.free_blocks;
    for (unsigned t = 0; t < sizeof sizes / sizeof sizes[0]; t++) {
        uint32_t n = sizes[t];
        uint8_t *w = malloc(n), *r = malloc(n + 16);
        pattern(w, n, t);
        char path[64];
        snprintf(path, sizeof path, "/big%u.bin", t);
        CHECK_EQ(nbfs_write_file(&fs, path, w, n), (int)n);
        remount();   /* force reads to come from "disk" through a fresh mount */
        memset(r, 0, n + 16);
        CHECK_EQ(nbfs_read_file(&fs, path, r, n + 16), (int)n);
        CHECK(!memcmp(w, r, n));
        struct nbfs_stat st;
        nbfs_stat(&fs, path, &st);
        CHECK_EQ(st.size, n);
        /* unaligned chunked writes produce identical content */
        int ino = nbfs_create(&fs, "/chunked.bin", NBFS_TYPE_FILE);
        for (uint32_t off = 0; off < n; off += 777) {
            uint32_t c = n - off < 777 ? n - off : 777;
            CHECK_EQ(nbfs_write(&fs, ino, off, w + off, c), (int)c);
        }
        memset(r, 0, n);
        CHECK_EQ(nbfs_read(&fs, ino, 0, r, n), (int)n);
        CHECK(!memcmp(w, r, n));
        CHECK_EQ(nbfs_unlink(&fs, "/chunked.bin"), 0);
        CHECK_EQ(nbfs_unlink(&fs, path), 0);
        CHECK_EQ(fs.sb.free_blocks, base_free);   /* every block came back */
        free(w); free(r);
    }
    fsck();
}

static void test_truncate(void)
{
    fresh(16);
    uint32_t n = 400 * 1024;
    uint8_t *w = malloc(n), *r = malloc(n);
    pattern(w, n, 9);
    int ino = nbfs_create(&fs, "/t.bin", NBFS_TYPE_FILE);
    uint32_t base_free = fs.sb.free_blocks;   /* after the root dir got its entry block */
    CHECK_EQ(nbfs_write(&fs, ino, 0, w, n), (int)n);
    CHECK_EQ(nbfs_truncate(&fs, ino, 300 * 1024 + 10), 0);   /* still in double-indirect range */
    fsck();
    CHECK_EQ(nbfs_truncate(&fs, ino, 100 * 1024 + 3), 0);    /* into single-indirect range */
    fsck();
    CHECK_EQ(nbfs_read(&fs, ino, 0, r, n), 100 * 1024 + 3);
    CHECK(!memcmp(w, r, 100 * 1024 + 3));
    /* grow again: the region past the old end must read back as zeros */
    CHECK_EQ(nbfs_write(&fs, ino, 200 * 1024, "Z", 1), 1);
    CHECK_EQ(nbfs_read(&fs, ino, 0, r, n), 200 * 1024 + 1);
    int zeros = 1;
    for (uint32_t i = 100 * 1024 + 3; i < 200 * 1024; i++) if (r[i]) { zeros = 0; break; }
    CHECK(zeros);
    CHECK_EQ(r[200 * 1024], 'Z');
    CHECK_EQ(nbfs_truncate(&fs, ino, 0), 0);
    CHECK_EQ(fs.sb.free_blocks, base_free);
    fsck();
    free(w); free(r);
}

static void test_directories(void)
{
    fresh(8);
    CHECK_EQ(nbfs_mkdir(&fs, "/docs"), 0);
    CHECK_EQ(nbfs_mkdir(&fs, "/docs/work"), 0);
    CHECK_EQ(nbfs_mkdir(&fs, "/docs/work/2024"), 0);
    CHECK_EQ(nbfs_mkdir(&fs, "/docs"), NBFS_EEXIST);
    CHECK_EQ(nbfs_mkdir(&fs, "/nope/sub"), NBFS_ENOENT);
    CHECK_EQ(nbfs_write_file(&fs, "/docs/work/2024/report.txt", "Q4", 2), 2);
    CHECK_EQ(nbfs_mkdir(&fs, "/docs/work/2024/report.txt/x"), NBFS_ENOTDIR);
    char buf[16] = { 0 };
    CHECK_EQ(nbfs_read_file(&fs, "/docs/./work/../work/2024/report.txt", buf, sizeof buf), 2);
    /* many entries: directory spans several blocks */
    for (int i = 0; i < 100; i++) {
        char p[64];
        snprintf(p, sizeof p, "/docs/file%03d.txt", i);
        CHECK(nbfs_create(&fs, p, NBFS_TYPE_FILE) > 0);
    }
    struct nbfs_stat st;
    nbfs_stat(&fs, "/docs", &st);
    uint32_t cookie = 0;
    struct nbfs_entry e;
    int count = 0, saw_work = 0;
    while (nbfs_readdir(&fs, st.inode, &cookie, &e) == 1) {
        count++;
        if (!strcmp(e.name, "work")) { saw_work = 1; CHECK_EQ(e.type, NBFS_TYPE_DIR); }
    }
    CHECK_EQ(count, 101);
    CHECK(saw_work);
    /* delete half, slots get reused */
    for (int i = 0; i < 100; i += 2) {
        char p[64];
        snprintf(p, sizeof p, "/docs/file%03d.txt", i);
        CHECK_EQ(nbfs_unlink(&fs, p), 0);
    }
    CHECK_EQ(nbfs_unlink(&fs, "/docs/file000.txt"), NBFS_ENOENT);
    uint32_t size_before = 0;
    nbfs_stat(&fs, "/docs", &st);
    size_before = st.size;
    CHECK(nbfs_create(&fs, "/docs/reuse.txt", NBFS_TYPE_FILE) > 0);
    nbfs_stat(&fs, "/docs", &st);
    CHECK_EQ(st.size, size_before);
    /* non-empty directories cannot be unlinked, but can be removed recursively */
    CHECK_EQ(nbfs_unlink(&fs, "/docs/work"), NBFS_ENOTEMPTY);
    CHECK_EQ(nbfs_unlink(&fs, "/"), NBFS_EINVAL);
    char path[NBFS_PATH_MAX];
    struct nbfs_stat rs;
    nbfs_stat(&fs, "/docs/work/2024", &rs);
    CHECK_EQ(nbfs_get_path(&fs, rs.inode, path, sizeof path), 0);
    CHECK(!strcmp(path, "/docs/work/2024"));
    CHECK_EQ(nbfs_remove_tree(&fs, "/docs"), 0);
    CHECK_EQ(nbfs_stat(&fs, "/docs", &st), NBFS_ENOENT);
    CHECK_EQ(fs.sb.free_inodes, fs.sb.inode_count - 2);
    fsck();
}

static void test_rename(void)
{
    fresh(8);
    nbfs_mkdir(&fs, "/a");
    nbfs_mkdir(&fs, "/a/b");
    nbfs_mkdir(&fs, "/c");
    nbfs_write_file(&fs, "/a/file.txt", "data", 4);
    CHECK_EQ(nbfs_rename(&fs, "/a/file.txt", "/a/renamed.txt"), 0);
    CHECK_EQ(nbfs_stat(&fs, "/a/file.txt", &(struct nbfs_stat){0}), NBFS_ENOENT);
    CHECK_EQ(nbfs_rename(&fs, "/a/renamed.txt", "/c/moved.txt"), 0);
    char buf[8] = { 0 };
    CHECK_EQ(nbfs_read_file(&fs, "/c/moved.txt", buf, 8), 4);
    CHECK(!strcmp(buf, "data"));
    CHECK_EQ(nbfs_rename(&fs, "/a", "/a/b/inside"), NBFS_EINVAL);   /* into itself */
    CHECK_EQ(nbfs_rename(&fs, "/a", "/c"), NBFS_EEXIST);
    CHECK_EQ(nbfs_rename(&fs, "/a", "/c/a2"), 0);                   /* move a directory */
    struct nbfs_stat st, pst;
    CHECK_EQ(nbfs_stat(&fs, "/c/a2/b", &st), 0);
    nbfs_stat(&fs, "/c/a2", &pst);
    CHECK_EQ(st.parent, pst.inode);
    CHECK_EQ(nbfs_rename(&fs, "/nope", "/x"), NBFS_ENOENT);
    fsck();
}

static void test_names_and_metadata(void)
{
    fresh(8);
    char longname[80];
    memset(longname, 'n', sizeof longname);
    longname[0] = '/';
    longname[NBFS_NAME_MAX + 1] = 0;            /* exactly NAME_MAX chars */
    CHECK(nbfs_create(&fs, longname, NBFS_TYPE_FILE) > 0);
    longname[NBFS_NAME_MAX + 1] = 'n';
    longname[NBFS_NAME_MAX + 2] = 0;            /* one too many */
    CHECK_EQ(nbfs_create(&fs, longname, NBFS_TYPE_FILE), NBFS_ENAMETOOLONG);
    CHECK_EQ(nbfs_create(&fs, "/", NBFS_TYPE_DIR), NBFS_EINVAL);
    CHECK(nbfs_create(&fs, "/Mixed Case & spaces.txt", NBFS_TYPE_FILE) > 0);
    fake_time = 1700000100;
    int ino = nbfs_create(&fs, "/stamp.txt", NBFS_TYPE_FILE);
    fake_time = 1700000500;
    nbfs_write(&fs, ino, 0, "x", 1);
    struct nbfs_stat st;
    nbfs_stat(&fs, "/stamp.txt", &st);
    CHECK_EQ(st.ctime, 1700000100);
    CHECK_EQ(st.mtime, 1700000500);
    CHECK_EQ(st.size, 1);
    CHECK_EQ(st.type, NBFS_TYPE_FILE);
    fsck();
}

static void test_full_disk(void)
{
    fresh(2);
    uint32_t chunk = 64 * 1024;
    uint8_t *w = malloc(chunk);
    pattern(w, chunk, 3);
    int ino = nbfs_create(&fs, "/fill.bin", NBFS_TYPE_FILE);
    uint32_t off = 0;
    int r;
    while ((r = nbfs_write(&fs, ino, off, w, chunk)) == (int)chunk) off += chunk;
    CHECK(r >= 0 || r == NBFS_ENOSPC);
    if (r > 0) off += r;
    CHECK_EQ(fs.sb.free_blocks, 0);
    CHECK_EQ(nbfs_write(&fs, ino, off, w, 10), NBFS_ENOSPC);
    struct nbfs_stat st;
    nbfs_stat(&fs, "/fill.bin", &st);
    CHECK_EQ(st.size, off);
    fsck();
    CHECK_EQ(nbfs_unlink(&fs, "/fill.bin"), 0);
    CHECK(fs.sb.free_blocks > 1000);
    fsck();
    free(w);
}

static void test_persistence(void)
{
    fresh(8);
    nbfs_mkdir(&fs, "/keep");
    nbfs_write_file(&fs, "/keep/me.txt", "persist", 7);
    uint32_t free_before = fs.sb.free_blocks;
    close(hd.fd);
    hd.fd = 0;
    /* reopen the image file from scratch */
    if (hostdev_open(img_path, &hd, &dev, 1)) exit(1);
    remount();
    char buf[16] = { 0 };
    CHECK_EQ(nbfs_read_file(&fs, "/keep/me.txt", buf, sizeof buf), 7);
    CHECK(!strcmp(buf, "persist"));
    CHECK_EQ(fs.sb.free_blocks, free_before);
    CHECK_EQ(strcmp(fs.sb.label, "TEST"), 0);
    fsck();
}

static void test_random_ops(void)
{
    /* randomized create/write/truncate/delete against an in-memory model */
    fresh(16);
    enum { N = 24 };
    static uint8_t model[N][20000];
    static uint32_t msize[N];
    static int exists[N];
    srand(42);
    for (int step = 0; step < 3000; step++) {
        int i = rand() % N;
        char p[32];
        snprintf(p, sizeof p, "/r%02d", i);
        int op = rand() % 4;
        if (op == 0 && !exists[i]) {
            CHECK(nbfs_create(&fs, p, NBFS_TYPE_FILE) > 0);
            exists[i] = 1; msize[i] = 0;
        } else if (op == 1 && exists[i]) {
            uint32_t off = rand() % 18000, n = rand() % 2000 + 1;
            uint8_t data[2048];
            for (uint32_t k = 0; k < n; k++) data[k] = rand();
            uint32_t ino;
            nbfs_lookup(&fs, p, &ino);
            CHECK_EQ(nbfs_write(&fs, ino, off, data, n), (int)n);
            if (off > msize[i]) memset(model[i] + msize[i], 0, off - msize[i]);
            memcpy(model[i] + off, data, n);
            if (off + n > msize[i]) msize[i] = off + n;
        } else if (op == 2 && exists[i]) {
            uint32_t ns = rand() % 20000, ino;
            nbfs_lookup(&fs, p, &ino);
            CHECK_EQ(nbfs_truncate(&fs, ino, ns), 0);
            if (ns > msize[i]) memset(model[i] + msize[i], 0, ns - msize[i]);
            msize[i] = ns;
        } else if (op == 3 && exists[i]) {
            CHECK_EQ(nbfs_unlink(&fs, p), 0);
            exists[i] = 0;
        }
    }
    for (int i = 0; i < N; i++) {
        if (!exists[i]) continue;
        char p[32];
        static uint8_t buf[20000];
        snprintf(p, sizeof p, "/r%02d", i);
        CHECK_EQ(nbfs_read_file(&fs, p, buf, sizeof buf), (int)msize[i]);
        CHECK(!memcmp(buf, model[i], msize[i]));
    }
    fsck();
}

static void test_paths(void)
{
    char out[NBFS_PATH_MAX];
    nbfs_path_normalize("a//b/./c/../d/", out, sizeof out);   CHECK(!strcmp(out, "/a/b/d"));
    nbfs_path_normalize("/../..", out, sizeof out);           CHECK(!strcmp(out, "/"));
    nbfs_path_join("/x/y", "../z", out, sizeof out);          CHECK(!strcmp(out, "/x/z"));
    nbfs_path_join("/x/y", "/abs", out, sizeof out);          CHECK(!strcmp(out, "/abs"));
    nbfs_path_parent("/x/y", out, sizeof out);                CHECK(!strcmp(out, "/x"));
    nbfs_path_parent("/", out, sizeof out);                   CHECK(!strcmp(out, "/"));
    CHECK(!strcmp(nbfs_path_basename("/x/y.txt"), "y.txt"));
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "paths", test_paths },
        { "format/mount", test_format_mount },
        { "create/read/write", test_create_read_write },
        { "large files (direct/indirect/double-indirect)", test_large_files },
        { "truncate", test_truncate },
        { "nested directories / readdir / delete", test_directories },
        { "rename / move", test_rename },
        { "names and timestamps", test_names_and_metadata },
        { "disk full", test_full_disk },
        { "persistence across remount", test_persistence },
        { "randomized operations vs model", test_random_ops },
    };
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].fn();
        printf("%-50s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    if (hd.fd > 0) close(hd.fd);
    unlink(img_path);
    return failures ? 1 : 0;
}
