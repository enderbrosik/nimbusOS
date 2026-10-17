/* Host test driver for gui/imgdec.c: decodes a file and writes "W H\n" and the pixels as raw RGB. */
#include "gui/imgdec.h"
#include <stdio.h>
surface_t *bmp_decode(const u8 *data, u32 len) { (void)data; (void)len; return NULL; }
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static u8 buf[1 << 24];
    u32 n = (u32)fread(buf, 1, sizeof buf, f);
    fclose(f);
    surface_t *s = img_decode(buf, n);
    if (!s) { fprintf(stderr, "decode failed\n"); return 1; }
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "%d %d\n", s->w, s->h);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) { u32 p = s->px[y * s->pitch + x]; fputc(p >> 16, o); fputc(p >> 8, o); fputc(p, o); }
    fclose(o);
    return 0;
}
