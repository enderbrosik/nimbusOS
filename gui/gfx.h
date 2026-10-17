#ifndef NIMBUS_GFX_H
#define NIMBUS_GFX_H
#include "kernel/types.h"

#define RGB(r, g, b) ((u32)(((r) << 16) | ((g) << 8) | (b)))
#define COLOR_NONE 0xFF000000u      /* "transparent" background for text */
#define FONT_W 8
#define FONT_H 16

typedef struct { int x, y, w, h; } rect_t;

typedef struct surface {
    u32 *px;
    int w, h;
    int pitch;          /* pixels per row */
    rect_t clip;        /* drawing is restricted to this rectangle */
    bool owns;
} surface_t;

/* rectangles */
static inline rect_t rect_make(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
static inline bool rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_contains(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
rect_t rect_intersect(rect_t a, rect_t b);
rect_t rect_union(rect_t a, rect_t b);
bool   rect_overlaps(rect_t a, rect_t b);

/* surfaces */
void       gfx_init(void);      /* loads the font */
surface_t *surface_create(int w, int h);
void       surface_wrap(surface_t *s, u32 *px, int w, int h, int pitch);
void       surface_destroy(surface_t *s);
void       gfx_set_clip(surface_t *s, rect_t r);   /* intersected with the surface bounds */
void       gfx_reset_clip(surface_t *s);

/* primitives (all clipped) */
void gfx_pixel(surface_t *s, int x, int y, u32 c);
u32  gfx_get_pixel(surface_t *s, int x, int y);
void gfx_hline(surface_t *s, int x, int y, int w, u32 c);
void gfx_vline(surface_t *s, int x, int y, int h, u32 c);
void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, u32 c);
void gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int r, u32 c);
void gfx_rect(surface_t *s, int x, int y, int w, int h, u32 c);
void gfx_fill_rect(surface_t *s, int x, int y, int w, int h, u32 c);
void gfx_blend_rect(surface_t *s, int x, int y, int w, int h, u32 c, int alpha);  /* alpha 0..255 */
void gfx_gradient_v(surface_t *s, int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_circle(surface_t *s, int cx, int cy, int r, u32 c);
void gfx_fill_circle(surface_t *s, int cx, int cy, int r, u32 c);
void gfx_round_rect(surface_t *s, int x, int y, int w, int h, int r, u32 c);   /* filled */

/* blits */
void gfx_blit(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h);
/* src pixels are 0xAARRGGBB; A=0 transparent, 0xFF opaque, otherwise blended */
void gfx_blit_argb(surface_t *dst, int dx, int dy, const u32 *src, int w, int h);
void gfx_blit_scaled(surface_t *dst, rect_t dr, surface_t *src);

/* text */
void gfx_char(surface_t *s, int x, int y, unsigned char ch, u32 fg, u32 bg);
int  gfx_text(surface_t *s, int x, int y, const char *str, u32 fg, u32 bg);
int  gfx_text_n(surface_t *s, int x, int y, const char *str, int n, u32 fg, u32 bg);
int  gfx_text_bold(surface_t *s, int x, int y, const char *str, u32 fg);
int  gfx_text_width(const char *str);
/* text magnified by an integer factor (big digits, card pips) */
int  gfx_text_scaled(surface_t *s, int x, int y, const char *str, int scale, u32 fg);
/* draws text clipped to max_w pixels, adding "..." if it does not fit */
void gfx_text_ellipsis(surface_t *s, int x, int y, const char *str, int max_w, u32 fg);
void gfx_text_centered(surface_t *s, rect_t r, const char *str, u32 fg);

/* UI font: proportional and anti-aliased (derived from the ROM font at boot).
 * Falls back to the plain monospace font when g_ui_smooth is false.
 * Editors and the terminal keep using the monospace gfx_text*. */
extern bool g_ui_smooth;
int  gfx_ui_text(surface_t *s, int x, int y, const char *str, u32 fg);
int  gfx_ui_text_bold(surface_t *s, int x, int y, const char *str, u32 fg);
int  gfx_ui_width(const char *str);
int  gfx_ui_width_n(const char *str, int n);
void gfx_ui_text_ellipsis(surface_t *s, int x, int y, const char *str, int max_w, u32 fg);
void gfx_ui_centered(surface_t *s, rect_t r, const char *str, u32 fg);

/* Explicit faces, for the browser: body text, bold, and 20 px headings. */
enum { UI_FACE_REG, UI_FACE_BOLD, UI_FACE_LARGE, UI_FACE_LARGE_BOLD, UI_FACE_COUNT };
int  gfx_ui_face_text(surface_t *s, int x, int y, const char *str, int face, u32 fg);
int  gfx_ui_face_text_italic(surface_t *s, int x, int y, const char *str, int face, u32 fg);   /* slanted */
int  gfx_ui_face_width(const char *str, int face);
int  gfx_ui_face_height(int face);
int  gfx_ui_face_ascent(int face);

u32 color_blend(u32 a, u32 b, int alpha);   /* alpha of b over a, 0..255 */
u32 color_lighten(u32 c, int amount);
u32 color_darken(u32 c, int amount);
#endif
