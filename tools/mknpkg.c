#include "npkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_name(const char *name)
{
    size_t len = strlen(name);
    if (!len || len > NPKG_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4 || !valid_name(argv[3])) {
        fprintf(stderr, "usage: mknpkg <program.elf> <output.npkg> <app-name>\n");
        return 1;
    }

    uint16_t endian = 1;
    if (*(uint8_t *)&endian != 1) {
        fprintf(stderr, "mknpkg: host must be little-endian\n");
        return 1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    if (fseek(in, 0, SEEK_END) || ftell(in) < 20) {
        fprintf(stderr, "mknpkg: invalid ELF file\n");
        fclose(in);
        return 1;
    }
    long size = ftell(in);
    rewind(in);
    unsigned char *elf = malloc((size_t)size);
    if (!elf) { perror("malloc"); fclose(in); return 1; }
    if (fread(elf, 1, (size_t)size, in) != (size_t)size) {
        perror(argv[1]); free(elf); fclose(in); return 1;
    }
    fclose(in);

    if (memcmp(elf, "\x7f" "ELF", 4) || elf[4] != 2 || elf[5] != 1 ||
        elf[16] != 2 || elf[17] != 0 || elf[18] != 62 || elf[19] != 0) {
        fprintf(stderr, "mknpkg: expected a static x86-64 ELF64 executable\n");
        free(elf);
        return 1;
    }

    struct npkg_header header = {0};
    memcpy(header.magic, NPKG_MAGIC, sizeof header.magic);
    header.version = NPKG_VERSION;
    header.name_len = (npkg_u32)strlen(argv[3]);
    header.elf_size = (npkg_u64)size;

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); free(elf); return 1; }
    int failed = fwrite(&header, sizeof header, 1, out) != 1 ||
                 fwrite(argv[3], 1, header.name_len, out) != header.name_len ||
                 fwrite(elf, 1, (size_t)size, out) != (size_t)size;
    if (fclose(out)) failed = 1;
    free(elf);
    if (failed) { perror(argv[2]); return 1; }
    printf("Created %s (%s, %ld-byte ELF)\n", argv[2], argv[3], size);
    return 0;
}