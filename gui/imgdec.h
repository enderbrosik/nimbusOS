/* NimbusOS - picture decoders for the Web Browser. */
#ifndef NIMBUS_IMGDEC_H
#define NIMBUS_IMGDEC_H
#include "gui/gfx.h"

/* Decodes a PNG (not interlaced), a GIF (first frame), a baseline JPEG or a BMP.
 * Returns a new surface, or NULL when the data is damaged or the kind is not
 * supported.  Transparent parts are blended with white.  The kernel has no
 * floating point, so everything here is integer arithmetic. */
surface_t *img_decode(const u8 *data, u32 len);
#endif
