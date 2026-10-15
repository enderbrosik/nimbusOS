#ifndef NIMBUS_BMP_H
#define NIMBUS_BMP_H
#include "gui/gfx.h"
/* Decode an uncompressed 24- or 32-bit BMP (BITMAPINFOHEADER) from memory. */
surface_t *bmp_decode(const u8 *data, u32 len);
surface_t *bmp_load(const char *path);
/* Encode a surface as a 24-bit BMP; returns kmalloc'ed buffer. */
u8 *bmp_encode(surface_t *s, u32 *out_len);
int bmp_save(const char *path, surface_t *s);
#endif
