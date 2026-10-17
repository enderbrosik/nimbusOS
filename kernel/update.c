/*
 * NimbusOS - Nimbus Update core (see kernel/update.h).
 *
 * The server is a static folder (tools/mkrelease.c):
 *   manifest.txt   "nimbus-update 1", version, date, notes, kernel and file lines
 *   manifest.sig   Ed25519 signature of manifest.txt
 *   files/<sha256> payloads
 * TLS in NimbusOS does not verify certificates, so trust comes only from
 * the signature: it must verify against the public key compiled into this
 * kernel, and every payload must match the SHA-256 listed in the signed
 * manifest.  Nothing is written until *all* payloads have been downloaded and
 * checked; the kernel goes to the inactive boot slot and is switched to last
 * (kernel/bootarea.c).
 */
#include "kernel/update.h"
#include "kernel/vercmp.h"
#include "kernel/version.h"
#include "kernel/bootarea.h"
#include "kernel/settings.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "fs/nbfs.h"
#include "net/net.h"
#include "net/netjob.h"
#include "net/crypto.h"
#include "update_pubkey.h"

#define MAX_ENTRIES 256

struct entry {
    bool kernel, needed;
    u8 sha[32];
    u32 size;
    char path[NBFS_PATH_MAX];
    u8 *data;
};

enum { STEP_NONE, STEP_MANIFEST, STEP_SIG, STEP_FILE };

struct update {
    enum update_phase phase;
    update_say_fn say;
    void *ctx;
    char status[192];
    char version[24], date[16];
    char *notes;
    char *manifest;
    u32 mlen;
    struct entry *e;
    int n, cur;
    struct netjob *job;
    int step;
    bool then_install, kernel_installed;
    u32 done, total;
    char base[256];
};

static void say(struct update *u, bool err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(struct update *u, bool err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->status, sizeof u->status, fmt, ap);
    va_end(ap);
    klog("[upd ] %s\n", u->status);
    if (u->say) u->say(u->ctx, err, u->status);
}

static void fail(struct update *u, const char *msg)
{
    say(u, true, "Update failed: %s", msg);
    u->phase = UPD_FAILED;
}

/* Nimbus Update only talks to the official site. The host is fixed at build
 * time (NIMBUS_UPDATE_HOST, nimbusos.vercel.app; the test suite builds
 * against a local server), and it cannot be changed from the settings. */
const char *update_server(void) { return NIMBUS_UPDATE_URL; }
bool update_host_allowed(const char *url)
{
    const char *host = strstr(url, "://");
    if (!host) return false;
    host += 3;
    u32 n = strlen(NIMBUS_UPDATE_HOST);
    return !strncasecmp(host, NIMBUS_UPDATE_HOST, n) && (host[n] == 0 || host[n] == '/' || host[n] == ':') &&
           !strncmp(url, "https://", 8);
}
void update_set_server(const char *url) { (void)url; }

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static bool parse_hex(const char *s, u8 *out, int n)
{
    for (int i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (u8)(a << 4 | b);
    }
    return true;
}

static bool pubkey(u8 out[32]) { return strlen(NIMBUS_UPDATE_PUBKEY) == 64 && parse_hex(NIMBUS_UPDATE_PUBKEY, out, 32); }
bool update_signing_configured(void) { u8 k[32]; return pubkey(k); }

static int version_cmp(const char *a, const char *b) { return nimbus_version_cmp(a, b); }

struct update *update_new(update_say_fn fn, void *ctx)
{
    struct update *u = kzalloc(sizeof *u);
    if (!u) return NULL;
    u->say = fn;
    u->ctx = ctx;
    u->e = kzalloc(MAX_ENTRIES * sizeof(struct entry));
    if (!u->e) { kfree(u); return NULL; }
    strlcpy(u->status, "Not checked yet", sizeof u->status);
    return u;
}

static void clear(struct update *u)
{
    if (u->job) { netjob_cancel(u->job); netjob_release(u->job); u->job = NULL; }
    for (int i = 0; i < u->n; i++) { kfree(u->e[i].data); u->e[i].data = NULL; }
    u->n = 0;
    kfree(u->manifest); u->manifest = NULL;
    kfree(u->notes); u->notes = NULL;
    u->version[0] = u->date[0] = 0;
    u->done = u->total = 0;
    u->kernel_installed = false;
}

void update_free(struct update *u)
{
    if (!u) return;
    clear(u);
    kfree(u->e);
    kfree(u);
}

static void fetch(struct update *u, const char *name, int step)
{
    char url[400];
    snprintf(url, sizeof url, "%s/%s", u->base, name);
    u->job = netjob_fetch(url);
    u->step = step;
}

void update_check(struct update *u, bool then_install)
{
    if (u->phase == UPD_CHECKING || u->phase == UPD_DOWNLOADING) return;
    clear(u);
    u->then_install = then_install;
    if (!update_signing_configured()) { fail(u, "this system has no update signing key (built without keys/update.pub)"); return; }
    strlcpy(u->base, update_server(), sizeof u->base);
    u32 l = strlen(u->base);
    while (l && u->base[l - 1] == '/') u->base[--l] = 0;
    if (!update_host_allowed(u->base)) { fail(u, "Nimbus Update only uses " NIMBUS_UPDATE_HOST); return; }
    u->phase = UPD_CHECKING;
    say(u, false, "Checking %s ...", u->base);
    fetch(u, "manifest.txt", STEP_MANIFEST);
}

/* Parse the verified manifest into entries; false on any malformed line. */
static bool parse_manifest(struct update *u)
{
    char *m = u->manifest;
    u32 notes_len = 0;
    u->notes = kzalloc(2048);
    bool magic = false;
    for (char *line = m; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strcmp(line, "nimbus-update 1")) magic = true;
        else if (!strncmp(line, "version ", 8)) strlcpy(u->version, line + 8, sizeof u->version);
        else if (!strncmp(line, "date ", 5)) strlcpy(u->date, line + 5, sizeof u->date);
        else if (!strncmp(line, "notes ", 6)) {
            u32 n = strlen(line + 6);
            if (notes_len + n + 2 < 2048) { memcpy(u->notes + notes_len, line + 6, n); notes_len += n; u->notes[notes_len++] = '\n'; }
        } else if (!strncmp(line, "kernel ", 7) || !strncmp(line, "file ", 5)) {
            if (u->n == MAX_ENTRIES) return false;
            struct entry *e = &u->e[u->n];
            bool k = line[0] == 'k';
            char *p = line + (k ? 7 : 5);
            if (strlen(p) < 66 || !parse_hex(p, e->sha, 32) || p[64] != ' ') return false;
            p += 65;
            e->size = (u32)atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            e->kernel = k;
            if (!k) {
                if (*p != ' ' || p[1] != '/') return false;
                strlcpy(e->path, p + 1, sizeof e->path);
                /* only system locations, never "..": the manifest cannot touch user files */
                if ((strncmp(e->path, "/Programs/", 10) && strncmp(e->path, "/System/", 8)) || strstr(e->path, "..")) return false;
            } else {
                strlcpy(e->path, "(kernel)", sizeof e->path);
            }
            u->n++;
        } else if (line[0]) {
            return false;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return magic && u->version[0];
}

/* Mark the entries that differ from what is installed. */
static void compute_needed(struct update *u)
{
    u->total = 0;
    for (int i = 0; i < u->n; i++) {
        struct entry *e = &u->e[i];
        u8 h[32];
        e->needed = true;
        if (e->kernel) {
            if (!bootarea_active_hash(e->size, h) && !memcmp(h, e->sha, 32)) e->needed = false;
        } else {
            char *data;
            u32 len;
            if (vfs_read_all(e->path, &data, &len) == 0) {
                sha256(data, len, h);
                if (len == e->size && !memcmp(h, e->sha, 32)) e->needed = false;
                kfree(data);
            }
        }
        if (e->needed) u->total += e->size;
    }
}

static void manifest_ready(struct update *u, char *sigtext)
{
    u8 sig[64], key[32];
    char *t = sigtext;
    while (*t == ' ' || *t == '\n' || *t == '\r') t++;
    if (strlen(t) < 128 || !parse_hex(t, sig, 64)) { fail(u, "the signature file is malformed"); return; }
    pubkey(key);
    if (ed25519_verify(sig, u->manifest, u->mlen, key)) {
        fail(u, "signature invalid - the update is not from the trusted publisher");
        return;
    }
    say(u, false, "Signature OK");
    if (!parse_manifest(u)) { fail(u, "the manifest is malformed"); return; }
    if (version_cmp(u->version, NIMBUS_VERSION) <= 0) {
        u->phase = UPD_UPTODATE;
        say(u, false, "NimbusOS %s is up to date (server has %s)", NIMBUS_VERSION, u->version);
        return;
    }
    compute_needed(u);
    int files = 0;
    bool kern = false;
    for (int i = 0; i < u->n; i++) if (u->e[i].needed) { if (u->e[i].kernel) kern = true; else files++; }
    u->phase = UPD_AVAILABLE;
    say(u, false, "NimbusOS %s is available (%d file%s%s, %u KiB)", u->version, files, files == 1 ? "" : "s",
        kern ? " and a new kernel" : "", (u->total + 1023) / 1024);
    if (u->then_install) update_install(u);
}

void update_install(struct update *u)
{
    if (u->phase != UPD_AVAILABLE) return;
    for (int i = 0; i < u->n; i++)
        if (u->e[i].needed && u->e[i].kernel && vfs_booted_from_ramdisk()) {
            fail(u, "running from the live CD; install NimbusOS to a disk first");
            return;
        }
    u->phase = UPD_DOWNLOADING;
    u->cur = 0;
    u->done = 0;
    say(u, false, "Downloading %u KiB ...", (u->total + 1023) / 1024);
}

static void mkdirs(const char *path)
{
    char dir[NBFS_PATH_MAX];
    strlcpy(dir, path, sizeof dir);
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        nbfs_mkdir(ROOTFS, dir);           /* EEXIST is fine */
        *p = '/';
    }
}

/* Everything has been downloaded and verified: write it. */
static void install_all(struct update *u)
{
    int files = 0;
    for (int i = 0; i < u->n; i++) {
        struct entry *e = &u->e[i];
        if (!e->needed || e->kernel) continue;
        mkdirs(e->path);
        int r = vfs_write_all(e->path, e->data, e->size);
        if (r) { char m[200]; snprintf(m, sizeof m, "writing %s: %s", e->path, nbfs_strerror(r)); fail(u, m); return; }
        say(u, false, "Installed %s", e->path);
        files++;
    }
    for (int i = 0; i < u->n; i++) {
        struct entry *e = &u->e[i];
        if (!e->needed || !e->kernel) continue;
        const char *why;
        if (bootarea_install_kernel(e->data, e->size, &why)) { fail(u, why); return; }
        u->kernel_installed = true;
        say(u, false, "Installed the new kernel");
    }
    struct rtc_time t;
    rtc_read(&t);
    char log[256];
    int n = snprintf(log, sizeof log, "Updated from %s to %s on %04d-%02d-%02d %02d:%02d UTC: %d files%s.\n",
                     NIMBUS_VERSION, u->version, t.year, t.month, t.day, t.hour, t.minute, files,
                     u->kernel_installed ? " and the kernel" : "");
    vfs_write_all("/System/update.log", log, n);
    u->phase = UPD_DONE;
    if (u->kernel_installed) say(u, false, "Update to %s installed - restart to finish", u->version);
    else say(u, false, "Update to %s installed", u->version);
}

void update_poll(struct update *u)
{
    if (u->phase == UPD_DOWNLOADING && !u->job) {
        while (u->cur < u->n && !u->e[u->cur].needed) u->cur++;
        if (u->cur == u->n) { say(u, false, "All files verified, installing ..."); install_all(u); return; }
        char name[80], hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", u->e[u->cur].sha[i]);
        snprintf(name, sizeof name, "files/%s", hex);
        fetch(u, name, STEP_FILE);
        return;
    }
    if (!u->job || !netjob_done(u->job)) return;
    char *body = NULL;
    u32 len = 0;
    int status = 0, err = 0;
    char final_url[256];
    netjob_fetch_result(u->job, &body, &len, &status, final_url, sizeof final_url, &err);
    netjob_release(u->job);
    u->job = NULL;
    if (err || status != 200) {
        char m[200];
        if (err) snprintf(m, sizeof m, "cannot reach the update server: %s", net_strerror(err));
        else snprintf(m, sizeof m, "server answered HTTP %d", status);
        kfree(body);
        fail(u, m);
        return;
    }
    switch (u->step) {
    case STEP_MANIFEST:
        if (len > 64 * 1024) { kfree(body); fail(u, "manifest too large"); return; }
        u->manifest = body;
        u->mlen = len;
        fetch(u, "manifest.sig", STEP_SIG);
        break;
    case STEP_SIG:
        manifest_ready(u, body);
        kfree(body);
        break;
    case STEP_FILE: {
        struct entry *e = &u->e[u->cur];
        u8 h[32];
        sha256(body, len, h);
        if (len != e->size || memcmp(h, e->sha, 32)) {
            kfree(body);
            char m[200];
            snprintf(m, sizeof m, "hash mismatch for %s - nothing was installed", e->path);
            fail(u, m);
            return;
        }
        e->data = (u8 *)body;
        u->done += len;
        say(u, false, "Verified %s (%u KiB)", e->path, (len + 1023) / 1024);
        u->cur++;
        break;
    }
    }
}

enum update_phase update_phase(struct update *u) { return u->phase; }
const char *update_status(struct update *u) { return u->status; }
const char *update_new_version(struct update *u) { return u->version; }
const char *update_notes(struct update *u) { return u->notes ? u->notes : ""; }
bool update_needs_restart(struct update *u) { return u->kernel_installed; }
void update_progress(struct update *u, u32 *done, u32 *total) { *done = u->done; *total = u->total; }
