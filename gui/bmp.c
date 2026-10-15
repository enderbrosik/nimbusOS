/*
 * NimbusOS - Windows BMP reader/writer.
 * Format: Microsoft "Bitmap Storage" (BITMAPFILEHEADER + BITMAPINFOHEADER),
 * uncompressed BI_RGB, 24 or 32 bpp, rows padded to 4 bytes, bottom-up
 * unless the height is negative.
 */
#include "gui/bmp.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/string.h"

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }

surface_t *bmp_decode(const u8 *d, u32 len)
{
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    u32 off = rd32(d + 10), hsize = rd32(d + 14);
    if (hsize < 40) return NULL;
    i32 w = (i32)rd32(d + 18), h = (i32)rd32(d + 22);
    u16 bpp = rd16(d + 28);
    u32 comp = rd32(d + 30);
    bool topdown = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    if ((bpp != 24 && bpp != 32) || (comp != 0 && !(comp == 3 && bpp == 32))) return NULL;
    u32 bytespp = bpp / 8;
    u32 row = (w * bytespp + 3) & ~3u;
    if (off + row * (u32)h > len) return NULL;
    surface_t *s = surface_create(w, h);
    if (!s) return NULL;
    for (i32 y = 0; y < h; y++) {
        const u8 *src = d + off + row * (topdown ? y : h - 1 - y);
        u32 *dst = s->px + y * s->pitch;
        for (i32 x = 0; x < w; x++, src += bytespp) dst[x] = RGB(src[2], src[1], src[0]);
    }
    return s;
}

surface_t *bmp_load(const char *path)
{
    char *data;
    u32 len;
    if (vfs_read_all(path, &data, &len) != 0) return NULL;
    surface_t *s = bmp_decode((u8 *)data, len);
    kfree(data);
    return s;
}

u8 *bmp_encode(surface_t *s, u32 *out_len)
{
    u32 row = (s->w * 3 + 3) & ~3u;
    u32 size = 54 + row * s->h;
    u8 *b = kzalloc(size);
    if (!b) return NULL;
    b[0] = 'B'; b[1] = 'M';
    wr32(b + 2, size);
    wr32(b + 10, 54);
    wr32(b + 14, 40);
    wr32(b + 18, s->w);
    wr32(b + 22, s->h);
    wr16(b + 26, 1);
    wr16(b + 28, 24);
    wr32(b + 34, row * s->h);
    wr32(b + 38, 2835);   /* 72 DPI */
    wr32(b + 42, 2835);
    for (int y = 0; y < s->h; y++) {
        u8 *dst = b + 54 + row * (s->h - 1 - y);
        u32 *src = s->px + y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            dst[x * 3] = src[x] & 0xFF;
            dst[x * 3 + 1] = (src[x] >> 8) & 0xFF;
            dst[x * 3 + 2] = (src[x] >> 16) & 0xFF;
        }
    }
    *out_len = size;
    return b;
}

int bmp_save(const char *path, surface_t *s)
{
    u32 len;
    u8 *b = bmp_encode(s, &len);
    if (!b) return NBFS_ENOSPC;
    int r = vfs_write_all(path, b, len);
    kfree(b);
    return r;
}
