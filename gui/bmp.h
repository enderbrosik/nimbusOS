#ifndef NIMBUS_BMP_H
#define NIMBUS_BMP_H
#include "gui/gfx.h"
/* Decode an uncompressed 24- or 32-bit BMP (BITMAPINFOHEADER) from memory. */
surface_t *bmp_decode(const u8 *data, u32 len);
surface_t *bmp_load(const char *path);
/* Encode a surface as a 24-bit BMP; returns kmalloc'ed buffer. */
u8 *bmp_encode(surface_t *s, u32 *out_len);
int bmp_save(const char *path, surface_t *s);
/* A pixel value that means "transparent" (not a colour; never produced by RGB()).
 * 32-bit BMP files carry it as alpha 0. */
#define PIXEL_CLEAR 0xFF000000u
/* Like bmp_save, but writes 32-bit BMP with alpha when the surface has transparent pixels. */
int bmp_save_alpha(const char *path, surface_t *s);
#endif
