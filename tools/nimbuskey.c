/*
 * NimbusOS - update signing key tool.
 *
 *   nimbuskey keygen <dir>    create <dir>/update.key (secret) and <dir>/update.pub
 *
 * The public key is compiled into the kernel (build/gen/update_pubkey.h);
 * the secret key signs releases (tools/mkrelease.c).  Keep update.key private
 * and backed up: without it no further updates can be published for
 * systems that trust update.pub.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "net/crypto.h"

static void put_hex(FILE *f, const u8 *p, int n)
{
    for (int i = 0; i < n; i++) fprintf(f, "%02x", p[i]);
    fprintf(f, "\n");
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "keygen")) { fprintf(stderr, "usage: nimbuskey keygen <dir>\n"); return 2; }
    char kpath[4096], ppath[4096];
    snprintf(kpath, sizeof kpath, "%s/update.key", argv[2]);
    snprintf(ppath, sizeof ppath, "%s/update.pub", argv[2]);
    struct stat st;
    if (stat(kpath, &st) == 0) { fprintf(stderr, "nimbuskey: %s already exists; not overwriting\n", kpath); return 1; }
    mkdir(argv[2], 0755);
    u8 seed[32], pub[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, seed, 32) != 32) { perror("/dev/urandom"); return 1; }
    close(fd);
    ed25519_public_key(pub, seed);
    int kfd = open(kpath, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *k = kfd >= 0 ? fdopen(kfd, "w") : NULL;
    FILE *p = fopen(ppath, "w");
    if (!k || !p) { perror("nimbuskey"); return 1; }
    put_hex(k, seed, 32);
    put_hex(p, pub, 32);
    fclose(k);
    fclose(p);
    printf("nimbuskey: wrote %s (keep secret) and %s\n", kpath, ppath);
    return 0;
}
