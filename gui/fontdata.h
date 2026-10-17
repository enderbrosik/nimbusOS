/* NimbusOS - UI font faces.  gui/fontdata.c is generated at build time by
 * tools/mkfont from the DejaVu TrueType fonts in fonts/.  Glyph i is code
 * point 32 + i, so the table covers ASCII and Latin-1. */
#ifndef NIMBUS_FONTDATA_H
#define NIMBUS_FONTDATA_H
#include "kernel/types.h"

struct ui_glyph {
    u8 adv;                 /* advance: pixels to move right after the glyph */
    u8 w, h;                /* coverage bitmap size (0 for blank glyphs) */
    i8 xoff, yoff;          /* top-left of the bitmap from the pen position, yoff from the line top */
    u16 off;                /* first byte of the bitmap in pix[] */
};

struct ui_face {
    u8 ascent;              /* line top to baseline, pixels */
    u8 height;              /* line height, pixels */
    const struct ui_glyph *glyphs;
    const u8 *pix;          /* 8-bit coverage, row-major */
};

extern const struct ui_face ui_face_regular, ui_face_bold;
#endif
