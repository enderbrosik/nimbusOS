/*
 * NimbusOS - 2D graphics primitives on 32-bit XRGB surfaces.
 *
 * All drawing honours the surface's clip rectangle, which is how the window
 * manager guarantees windows never draw outside their bounds.
 * Algorithms: Bresenham's line algorithm and the midpoint circle algorithm
 * (Foley & van Dam, "Computer Graphics: Principles and Practice", 3.2/3.3).
 *
 * The 8x16 font is the VGA BIOS ROM font, copied by stage 2 from the
 * pointer returned by INT 10h AX=1130h BH=06h (code page 437 glyphs).
 */
#include "gui/gfx.h"
#include "gui/fontdata.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/bootinfo.h"
#include "kernel/kprintf.h"

static u8 font[256 * 16];
static bool font_ok;

/* ---- UI font -------------------------------------------------------------
 * Proportional, anti-aliased DejaVu Sans (regular and bold, 14 px).  The
 * outlines are rasterised on the host by tools/mkfont, because the kernel has
 * no floating point; gui/fontdata.h describes the tables it writes. */
bool g_ui_smooth = true;
void gfx_init(void)
{
    if (g_bootinfo.font_addr) {
        memcpy(font, (void *)(uptr)g_bootinfo.font_addr, sizeof font);
        font_ok = true;
    } else {
        /* No ROM font: render every glyph as a hollow box. */
        for (int c = 33; c < 256; c++)
            for (int y = 3; y < 13; y++)
                font[c * 16 + y] = (y == 3 || y == 12) ? 0x7E : 0x42;
    }
    klog("[gfx ] font %s; UI font: DejaVu Sans %d px, anti-aliased\n",
         font_ok ? "loaded from VGA ROM" : "missing, using boxes", ui_face_regular.height);
}

rect_t rect_intersect(rect_t a, rect_t b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

rect_t rect_union(rect_t a, rect_t b)
{
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool rect_overlaps(rect_t a, rect_t b) { return !rect_empty(rect_intersect(a, b)); }

surface_t *surface_create(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    surface_t *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->px = kmalloc((size_t)w * h * 4);
    if (!s->px) { kfree(s); return NULL; }
    s->w = w; s->h = h; s->pitch = w; s->owns = true;
    gfx_reset_clip(s);
    return s;
}

void surface_wrap(surface_t *s, u32 *px, int w, int h, int pitch)
{
    s->px = px; s->w = w; s->h = h; s->pitch = pitch; s->owns = false;
    gfx_reset_clip(s);
}

void surface_destroy(surface_t *s)
{
    if (!s) return;
    if (s->owns) kfree(s->px);
    kfree(s);
}

void gfx_reset_clip(surface_t *s) { s->clip = rect_make(0, 0, s->w, s->h); }
void gfx_set_clip(surface_t *s, rect_t r) { s->clip = rect_intersect(r, rect_make(0, 0, s->w, s->h)); }

void gfx_pixel(surface_t *s, int x, int y, u32 c)
{
    if (rect_contains(s->clip, x, y)) s->px[y * s->pitch + x] = c;
}

u32 gfx_get_pixel(surface_t *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->px[y * s->pitch + x];
}

void gfx_fill_rect(surface_t *s, int x, int y, int w, int h, u32 c)
{
    rect_t r = rect_intersect(rect_make(x, y, w, h), s->clip);
    if (rect_empty(r)) return;
    u32 *row = s->px + r.y * s->pitch + r.x;
    for (int j = 0; j < r.h; j++, row += s->pitch) memset32(row, c, r.w);
}

void gfx_hline(surface_t *s, int x, int y, int w, u32 c) { gfx_fill_rect(s, x, y, w, 1, c); }
void gfx_vline(surface_t *s, int x, int y, int h, u32 c) { gfx_fill_rect(s, x, y, 1, h, c); }

void gfx_rect(surface_t *s, int x, int y, int w, int h, u32 c)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

u32 color_blend(u32 a, u32 b, int al)
{
    u32 rb = ((a & 0xFF00FF) * (255 - al) + (b & 0xFF00FF) * al) >> 8;
    u32 g = ((a & 0x00FF00) * (255 - al) + (b & 0x00FF00) * al) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

u32 color_lighten(u32 c, int amt) { return color_blend(c, 0xFFFFFF, amt); }
u32 color_darken(u32 c, int amt) { return color_blend(c, 0x000000, amt); }

void gfx_blend_rect(surface_t *s, int x, int y, int w, int h, u32 c, int alpha)
{
    rect_t r = rect_intersect(rect_make(x, y, w, h), s->clip);
    for (int j = 0; j < r.h; j++) {
        u32 *row = s->px + (r.y + j) * s->pitch + r.x;
        for (int i = 0; i < r.w; i++) row[i] = color_blend(row[i], c, alpha);
    }
}

void gfx_gradient_v(surface_t *s, int x, int y, int w, int h, u32 top, u32 bottom)
{
    for (int j = 0; j < h; j++)
        gfx_hline(s, x, y + j, w, color_blend(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}

void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, u32 c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int r, u32 c)
{
    if (r <= 0) { gfx_line(s, x0, y0, x1, y1, c); return; }
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_fill_circle(s, x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(surface_t *s, int cx, int cy, int r, u32 c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(s, cx + x, cy + y, c); gfx_pixel(s, cx - x, cy + y, c);
        gfx_pixel(s, cx + x, cy - y, c); gfx_pixel(s, cx - x, cy - y, c);
        gfx_pixel(s, cx + y, cy + x, c); gfx_pixel(s, cx - y, cy + x, c);
        gfx_pixel(s, cx + y, cy - x, c); gfx_pixel(s, cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_fill_circle(surface_t *s, int cx, int cy, int r, u32 c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_hline(s, cx - x, cy + y, 2 * x + 1, c);
        gfx_hline(s, cx - x, cy - y, 2 * x + 1, c);
        gfx_hline(s, cx - y, cy + x, 2 * y + 1, c);
        gfx_hline(s, cx - y, cy - x, 2 * y + 1, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_round_rect(surface_t *s, int x, int y, int w, int h, int r, u32 c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_fill_rect(s, x, y + r, w, h - 2 * r, c);
    for (int j = 0; j < r; j++) {
        /* inset of row j from the corner arc: r - sqrt(r^2 - (r-j)^2) */
        int dy = r - j, dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        int inset = r - dx;
        gfx_hline(s, x + inset, y + j, w - 2 * inset, c);
        gfx_hline(s, x + inset, y + h - 1 - j, w - 2 * inset, c);
    }
}

void gfx_blit(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h)
{
    rect_t r = rect_intersect(rect_make(dx, dy, w, h), dst->clip);
    if (rect_empty(r)) return;
    sx += r.x - dx; sy += r.y - dy;
    /* also clip against the source surface */
    if (sx < 0) { r.x -= sx; r.w += sx; sx = 0; }
    if (sy < 0) { r.y -= sy; r.h += sy; sy = 0; }
    if (sx + r.w > src->w) r.w = src->w - sx;
    if (sy + r.h > src->h) r.h = src->h - sy;
    if (r.w <= 0 || r.h <= 0) return;
    for (int j = 0; j < r.h; j++)
        memcpy(dst->px + (r.y + j) * dst->pitch + r.x, src->px + (sy + j) * src->pitch + sx, r.w * 4);
}

void gfx_blit_argb(surface_t *dst, int dx, int dy, const u32 *src, int w, int h)
{
    rect_t r = rect_intersect(rect_make(dx, dy, w, h), dst->clip);
    for (int j = r.y; j < r.y + r.h; j++) {
        const u32 *sp = src + (j - dy) * w + (r.x - dx);
        u32 *dp = dst->px + j * dst->pitch + r.x;
        for (int i = 0; i < r.w; i++) {
            u32 a = sp[i] >> 24;
            if (a == 0xFF) dp[i] = sp[i] & 0xFFFFFF;
            else if (a) dp[i] = color_blend(dp[i], sp[i] & 0xFFFFFF, a);
        }
    }
}

void gfx_blit_scaled(surface_t *dst, rect_t dr, surface_t *src)
{
    rect_t r = rect_intersect(dr, dst->clip);
    if (rect_empty(r) || dr.w <= 0 || dr.h <= 0) return;
    for (int j = r.y; j < r.y + r.h; j++) {
        int sy = (j - dr.y) * src->h / dr.h;
        u32 *dp = dst->px + j * dst->pitch;
        u32 *sp = src->px + sy * src->pitch;
        for (int i = r.x; i < r.x + r.w; i++) dp[i] = sp[(i - dr.x) * src->w / dr.w];
    }
}

void gfx_char(surface_t *s, int x, int y, unsigned char ch, u32 fg, u32 bg)
{
    rect_t cell = rect_intersect(rect_make(x, y, FONT_W, FONT_H), s->clip);
    if (rect_empty(cell)) return;
    const u8 *g = font + ch * 16;
    for (int j = cell.y - y; j < cell.y - y + cell.h; j++) {
        u8 bits = g[j];
        u32 *row = s->px + (y + j) * s->pitch;
        for (int i = cell.x - x; i < cell.x - x + cell.w; i++) {
            if (bits & (0x80 >> i)) row[x + i] = fg;
            else if (bg != COLOR_NONE) row[x + i] = bg;
        }
    }
}

int gfx_text_n(surface_t *s, int x, int y, const char *str, int n, u32 fg, u32 bg)
{
    int x0 = x;
    for (int i = 0; i < n && str[i]; i++) {
        if (str[i] == '\t') { x += FONT_W * 4; continue; }
        gfx_char(s, x, y, (unsigned char)str[i], fg, bg);
        x += FONT_W;
    }
    return x - x0;
}

int gfx_text(surface_t *s, int x, int y, const char *str, u32 fg, u32 bg)
{
    return gfx_text_n(s, x, y, str, 0x7FFFFFFF, fg, bg);
}

int gfx_text_bold(surface_t *s, int x, int y, const char *str, u32 fg)
{
    gfx_text(s, x, y, str, fg, COLOR_NONE);
    return gfx_text(s, x + 1, y, str, fg, COLOR_NONE) + 1;
}

int gfx_text_width(const char *str)
{
    int w = 0;
    for (; *str; str++) w += *str == '\t' ? FONT_W * 4 : FONT_W;
    return w;
}

void gfx_text_ellipsis(surface_t *s, int x, int y, const char *str, int max_w, u32 fg)
{
    int len = strlen(str);
    int fit = max_w / FONT_W;
    if (len <= fit) { gfx_text(s, x, y, str, fg, COLOR_NONE); return; }
    if (fit <= 3) { gfx_text_n(s, x, y, "...", fit, fg, COLOR_NONE); return; }
    gfx_text_n(s, x, y, str, fit - 3, fg, COLOR_NONE);
    gfx_text(s, x + (fit - 3) * FONT_W, y, "...", fg, COLOR_NONE);
}

void gfx_text_centered(surface_t *s, rect_t r, const char *str, u32 fg)
{
    int w = gfx_text_width(str);
    gfx_text(s, r.x + (r.w - w) / 2, r.y + (r.h - FONT_H) / 2, str, fg, COLOR_NONE);
}

int gfx_text_scaled(surface_t *s, int x, int y, const char *str, int scale, u32 fg)
{
    int x0 = x;
    for (; *str; str++, x += FONT_W * scale) {
        const u8 *g = font + (u8)*str * 16;
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (0x80 >> i)) gfx_fill_rect(s, x + i * scale, y + j * scale, scale, scale, fg);
    }
    return x - x0;
}

/* The glyph for a byte; control characters and bytes past Latin-1 use the space. */
static const struct ui_glyph *ui_glyph(const struct ui_face *f, unsigned char ch)
{
    return &f->glyphs[ch < 32 ? 0 : ch - 32];
}

/* Draws one glyph with its top-left at (x, y) (line top) and returns its advance.
 * Coverage 255 is solid, 0 leaves the pixel alone, anything between blends. */
static int ui_char(surface_t *s, const struct ui_face *f, int x, int y, unsigned char ch, u32 fg)
{
    const struct ui_glyph *g = ui_glyph(f, ch);
    if (!g->w) return g->adv;
    int gx = x + g->xoff, gy = y + g->yoff;
    rect_t r = rect_intersect(rect_make(gx, gy, g->w, g->h), s->clip);
    for (int j = r.y; j < r.y + r.h; j++) {
        u32 *row = s->px + j * s->pitch;
        const u8 *src = f->pix + g->off + (j - gy) * g->w;
        for (int i = r.x; i < r.x + r.w; i++) {
            int a = src[i - gx];
            if (a == 255) row[i] = fg;
            else if (a) row[i] = color_blend(row[i], fg, a);
        }
    }
    return g->adv;
}

int gfx_ui_text(surface_t *s, int x, int y, const char *str, u32 fg)
{
    if (!g_ui_smooth) return gfx_text(s, x, y, str, fg, COLOR_NONE);
    int x0 = x;
    for (; *str; str++) x += *str == '\t' ? 16 : ui_char(s, &ui_face_regular, x, y, (unsigned char)*str, fg);
    return x - x0;
}

int gfx_ui_text_bold(surface_t *s, int x, int y, const char *str, u32 fg)
{
    if (!g_ui_smooth) return gfx_text_bold(s, x, y, str, fg);
    int x0 = x;
    for (; *str; str++) x += *str == '\t' ? 16 : ui_char(s, &ui_face_bold, x, y, (unsigned char)*str, fg);
    return x - x0;
}

int gfx_ui_width_n(const char *str, int n)
{
    if (!g_ui_smooth) { int l = strlen(str); return (n < l ? n : l) * FONT_W; }
    int w = 0;
    for (int i = 0; i < n && str[i]; i++)
        w += str[i] == '\t' ? 16 : ui_glyph(&ui_face_regular, (unsigned char)str[i])->adv;
    return w;
}

int gfx_ui_width(const char *str) { return gfx_ui_width_n(str, 0x7FFFFFFF); }

void gfx_ui_text_ellipsis(surface_t *s, int x, int y, const char *str, int max_w, u32 fg)
{
    if (gfx_ui_width(str) <= max_w) { gfx_ui_text(s, x, y, str, fg); return; }
    int dots = gfx_ui_width("...");
    int n = strlen(str);
    while (n > 0 && gfx_ui_width_n(str, n) + dots > max_w) n--;
    char buf[256];
    if (n > 250) n = 250;
    memcpy(buf, str, n);
    strcpy(buf + n, "...");
    gfx_ui_text(s, x, y, buf, fg);
}

void gfx_ui_centered(surface_t *s, rect_t r, const char *str, u32 fg)
{
    gfx_ui_text(s, r.x + (r.w - gfx_ui_width(str)) / 2, r.y + (r.h - FONT_H) / 2, str, fg);
}
