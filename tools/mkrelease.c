/*
 * NimbusOS - build a signed update release for Nimbus Update.
 *
 *   mkrelease <out-dir> <version> <kernel.bin> <update.key> <notes.txt> <root>...
 *
 * Every <root> is a folder whose contents map to "/" on the system volume
 * (rootfs/, the generated wallpapers, the user programs).  Only system files
 * are shipped - /Programs/... and /System/... except per-machine settings -
 * so updates never overwrite the user's documents.  Output:
 *
 *   <out>/manifest.txt    "nimbus-update 1", version, date, notes, kernel and file lines
 *   <out>/manifest.sig    Ed25519 signature of manifest.txt (hex)
 *   <out>/files/<sha256>  every payload, named by its SHA-256
 *   <out>/vercel.json     static hosting settings; <out>/index.html a landing page
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "net/crypto.h"

struct entry { char path[1024]; char src[4096]; };
static struct entry *ents;
static int nents, cap;
static const char *out_dir;

static int shipped(const char *p)
{
    if (!strcmp(p, "/System/settings.cfg") || !strcmp(p, "/System/update.log") ||
        !strcmp(p, "/System/installed.txt")) return 0;
    return !strncmp(p, "/Programs/", 10) || !strncmp(p, "/System/", 8);
}

static void walk(const char *host, const char *path)
{
    DIR *d = opendir(host);
    if (!d) { perror(host); exit(1); }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char h[4096], p[1024];
        snprintf(h, sizeof h, "%s/%s", host, de->d_name);
        snprintf(p, sizeof p, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(h, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(h, p); continue; }
        if (!S_ISREG(st.st_mode) || !shipped(p)) continue;
        for (int i = 0; i < nents; i++)        /* later roots override earlier ones */
            if (!strcmp(ents[i].path, p)) { strcpy(ents[i].src, h); goto next; }
        if (nents == cap) { cap = cap ? cap * 2 : 64; ents = realloc(ents, cap * sizeof *ents); }
        strcpy(ents[nents].path, p);
        strcpy(ents[nents].src, h);
        nents++;
    next:;
    }
    closedir(d);
}

static int cmp(const void *a, const void *b) { return strcmp(((const struct entry *)a)->path, ((const struct entry *)b)->path); }

static unsigned char *slurp(const char *f, long *len)
{
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); exit(1); }
    fseek(fp, 0, SEEK_END);
    *len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *b = malloc(*len + 1);
    if (fread(b, 1, *len, fp) != (size_t)*len) { perror(f); exit(1); }
    fclose(fp);
    b[*len] = 0;
    return b;
}

static void hexstr(char *out, const u8 *p, int n) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]); }

/* write the payload as files/<sha256>, return the hash as hex */
static void store(const char *src, char hex[65], long *size)
{
    unsigned char *b = slurp(src, size);
    u8 h[32];
    sha256(b, *size, h);
    hexstr(hex, h, 32);
    char dst[4096];
    snprintf(dst, sizeof dst, "%s/files/%s", out_dir, hex);
    FILE *f = fopen(dst, "wb");
    if (!f || fwrite(b, 1, *size, f) != (size_t)*size) { perror(dst); exit(1); }
    fclose(f);
    free(b);
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: mkrelease <out-dir> <version> <kernel.bin> <update.key> <notes.txt> <root>...\n");
        return 2;
    }
    out_dir = argv[1];
    const char *version = argv[2];
    char path[4096];
    mkdir(out_dir, 0755);
    snprintf(path, sizeof path, "%s/files", out_dir);
    mkdir(path, 0755);

    /* secret key: 64 hex characters */
    long kl;
    unsigned char *khex = slurp(argv[4], &kl);
    u8 seed[32], pub[32];
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf((char *)khex + 2 * i, "%2x", &v) != 1) { fprintf(stderr, "mkrelease: bad key file\n"); return 1; }
        seed[i] = (u8)v;
    }
    ed25519_public_key(pub, seed);

    for (int i = 6; i < argc; i++) walk(argv[i], "");
    qsort(ents, nents, sizeof *ents, cmp);

    size_t mcap = 65536, mlen = 0;
    char *m = malloc(mcap);
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    mlen += sprintf(m + mlen, "nimbus-update 1\nversion %s\ndate %04d-%02d-%02d\n", version,
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    FILE *nf = fopen(argv[5], "r");
    if (nf) {
        char line[512];
        while (fgets(line, sizeof line, nf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] && line[0] != '#') mlen += snprintf(m + mlen, mcap - mlen, "notes %s\n", line);
        }
        fclose(nf);
    }
    char hex[65];
    long size;
    store(argv[3], hex, &size);
    mlen += snprintf(m + mlen, mcap - mlen, "kernel %s %ld\n", hex, size);
    for (int i = 0; i < nents; i++) {
        store(ents[i].src, hex, &size);
        mlen += snprintf(m + mlen, mcap - mlen, "file %s %ld %s\n", hex, size, ents[i].path);
        if (mlen > mcap - 2048) { mcap *= 2; m = realloc(m, mcap); }
    }

    u8 sig[64];
    ed25519_sign(sig, m, mlen, seed, pub);
    if (ed25519_verify(sig, m, mlen, pub)) { fprintf(stderr, "mkrelease: signature self-check failed\n"); return 1; }
    char sighex[129];
    hexstr(sighex, sig, 64);

    snprintf(path, sizeof path, "%s/manifest.txt", out_dir);
    FILE *f = fopen(path, "wb");
    fwrite(m, 1, mlen, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/manifest.sig", out_dir);
    f = fopen(path, "w");
    fprintf(f, "%s\n", sighex);
    fclose(f);
    snprintf(path, sizeof path, "%s/vercel.json", out_dir);
    f = fopen(path, "w");
    fprintf(f, "{\n  \"headers\": [\n    { \"source\": \"/(manifest|release-notes)(.*)\", \"headers\": [ { \"key\": \"Cache-Control\", \"value\": \"no-cache\" } ] }\n  ]\n}\n");
    fclose(f);
    snprintf(path, sizeof path, "%s/index.html", out_dir);
    f = fopen(path, "w");
    fprintf(f, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>NimbusOS updates</title></head><body>"
               "<h1>NimbusOS update server</h1><p>Current release: <b>%s</b>. NimbusOS systems download "
               "<a href=\"manifest.txt\">manifest.txt</a> from here with Nimbus Update.</p></body></html>\n", version);
    fclose(f);
    char pubhex[65];
    hexstr(pubhex, pub, 32);
    printf("mkrelease: version %s, kernel + %d files, signed by %.16s...\n", version, nents, pubhex);
    return 0;
}
