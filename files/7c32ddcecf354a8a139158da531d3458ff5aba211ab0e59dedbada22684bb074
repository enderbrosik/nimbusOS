/* wc <file>... - count lines, words and bytes (reads files via SYS_READFILE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <nimbus.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: wc <file>...\n"); return 1; }
    long tl = 0, tw = 0, tb = 0;
    int status = 0;
    for (int i = 1; i < argc; i++) {
        long size = readfile(argv[i], NULL, 0);
        if (size < 0) { fprintf(stderr, "wc: %s: %s\n", argv[i], strerror((int)size)); status = 1; continue; }
        char *buf = malloc(size + 1);
        if (!buf) { fprintf(stderr, "wc: out of memory\n"); return 1; }
        readfile(argv[i], buf, size);
        long lines = 0, words = 0;
        int in_word = 0;
        for (long k = 0; k < size; k++) {
            if (buf[k] == '\n') lines++;
            if (isspace((unsigned char)buf[k])) in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
        printf("%7ld %7ld %7ld %s\n", lines, words, size, argv[i]);
        tl += lines; tw += words; tb += size;
        free(buf);
    }
    if (argc > 2) printf("%7ld %7ld %7ld total\n", tl, tw, tb);
    return status;
}
