#ifndef HOSTSHIM_GFX_H
#define HOSTSHIM_GFX_H
#include "kernel/types.h"
#include <stdlib.h>
typedef struct { u32 *px; int w, h, pitch; } surface_t;
#define RGB(r, g, b) ((u32)(((r) << 16) | ((g) << 8) | (b)))
static inline surface_t *surface_create(int w, int h)
{
    surface_t *s = malloc(sizeof *s);
    s->px = calloc((size_t)w * h, 4);
    s->w = w; s->h = h; s->pitch = w;
    return s;
}
static inline void surface_destroy(surface_t *s) { if (s) { free(s->px); free(s); } }
static inline void gfx_fill_rect(surface_t *s, int x, int y, int w, int h, u32 c)
{
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) s->px[j * s->pitch + i] = c;
}
#endif
