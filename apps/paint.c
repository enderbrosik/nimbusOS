/*
 * NimbusOS - Paint.
 *
 * Tools: pencil, brush, eraser, line, rectangle, filled rectangle, flood
 * fill and colour picker; a palette with a transparent pen (the first swatch,
 * drawn as a checkerboard) and 28 colours (left click = primary colour,
 * right click = secondary colour); brush sizes; Clear; Size, which changes the
 * canvas size from presets (32x32 up to 3840x2160) or a typed size.  Pictures
 * are saved as uncompressed BMP files: 24-bit, or 32-bit with alpha when the
 * picture has transparent pixels.  Shapes show a live preview that is only
 * committed to the canvas when the mouse button is released.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/dialog.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "gui/bmp.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define TOOLBAR_H 32
#define TOOLBOX_W 64
#define PALETTE_H 48
#define STATUS_H 20
#define SB 14

enum { T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_FILLRECT, T_FILL, T_PICK, NTOOLS };
static const char *tool_names[NTOOLS] = { "Pencil", "Brush", "Eraser", "Line", "Rectangle", "Filled Rectangle", "Fill", "Pick Colour" };
enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, B_CLEAR, B_SIZE, NBTN };
enum { AFTER_NONE, AFTER_NEW, AFTER_OPEN, AFTER_CLOSE };

#define NPAL 29
static const u32 palette[NPAL] = {
    PIXEL_CLEAR,   /* transparent pen */
    0x000000, 0x7F7F7F, 0x880015, 0xED1C24, 0xFF7F27, 0xFFF200, 0x22B14C, 0x00A2E8, 0x3F48CC, 0xA349A4,
    0x6B3E1E, 0x2F4F4F, 0x004080, 0x408080,
    0xFFFFFF, 0xC3C3C3, 0xB97A57, 0xFFAEC9, 0xFFC90E, 0xEFE4B0, 0xB5E61D, 0x99D9EA, 0x7092BE, 0xC8BFE7,
    0xF0B070, 0x80FF80, 0x80C0FF, 0xFF80C0,
};
static const int sizes[4] = { 1, 3, 6, 10 };

/* canvas size presets, in the order the Size dialog shows them */
static const int preset_w[] = { 32, 64, 128, 256, 512, 640, 800, 1024, 1280, 1920, 3840 };
static const int preset_h[] = { 32, 64, 128, 256, 512, 400, 600, 768, 720, 1080, 2160 };
#define NPRESET ((int)(sizeof preset_w / sizeof preset_w[0]))
#define MAX_CANVAS_W 3840
#define MAX_CANVAS_H 2160

/* light checkerboard: what a transparent pixel looks like on screen */
static u32 checker(int x, int y, int shift)
{
    return ((x >> shift) + (y >> shift)) & 1 ? 0xCCCCCC : 0xFFFFFF;
}

/* a colour swatch; transparent colours show the checkerboard */
static void draw_swatch(surface_t *s, rect_t r, u32 c)
{
    if (c != PIXEL_CLEAR) { gfx_fill_rect(s, r.x, r.y, r.w, r.h, c); return; }
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) gfx_fill_rect(s, x, y, 1, 1, checker(x, y, 2));
}

/* copies the canvas into the view (clipped); transparent pixels show the checkerboard */
static void draw_canvas(surface_t *s, int x0, int y0, surface_t *src)
{
    rect_t r = rect_intersect(rect_make(x0, y0, src->w, src->h), s->clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        const u32 *sp = src->px + (y - y0) * src->pitch + (r.x - x0);
        u32 *dp = s->px + y * s->pitch + r.x;
        for (int x = 0; x < r.w; x++) {
            u32 c = sp[x];
            dp[x] = c == PIXEL_CLEAR ? checker(r.x + x - x0, y - y0, 3) : c;
        }
    }
}

struct paint {
    surface_t *canvas;
    int tool, size_idx;
    u32 fg, bg;
    int ox, oy;                  /* canvas scroll offset */
    bool drawing;
    int draw_button;
    int sx, sy, lx, ly, cx, cy;  /* start, last, current (canvas coords) */
    bool mouse_in;
    char path[NBFS_PATH_MAX];
    bool dirty;
    int after_save;
    ui_btn_t btn[NBTN];
    int hover, pressed;
    int drag_sb;                 /* 0 none, 1 vertical, 2 horizontal */
};

static rect_t view_rect(window_t *w)
{
    return rect_make(TOOLBOX_W, TOOLBAR_H, w->surface->w - TOOLBOX_W - SB,
                     w->surface->h - TOOLBAR_H - PALETTE_H - STATUS_H - SB);
}

static void clamp_offset(window_t *w)
{
    struct paint *p = w->app;
    rect_t v = view_rect(w);
    p->ox = MAX(0, MIN(p->ox, p->canvas->w - v.w + 8));
    p->oy = MAX(0, MIN(p->oy, p->canvas->h - v.h + 8));
    if (p->canvas->w + 8 <= v.w) p->ox = 0;
    if (p->canvas->h + 8 <= v.h) p->oy = 0;
}

/* canvas position of the top-left of the view (canvas drawn with 4px margin) */
static int canvas_x(window_t *w) { struct paint *p = w->app; return view_rect(w).x + 4 - p->ox; }
static int canvas_y(window_t *w) { struct paint *p = w->app; return view_rect(w).y + 4 - p->oy; }

static void update_title(window_t *w)
{
    struct paint *p = w->app;
    char t[96];
    snprintf(t, sizeof t, "%s%s - Paint", p->dirty ? "*" : "", p->path[0] ? nbfs_path_basename(p->path) : "Untitled");
    if (strcmp(t, w->title)) wm_set_title(w, t);
}

static void tool_glyph(surface_t *s, int t, int x, int y, u32 c)
{
    switch (t) {
    case T_PENCIL:
        gfx_line(s, x + 3, y + 14, x + 13, y + 4, c); gfx_line(s, x + 4, y + 15, x + 14, y + 5, c);
        gfx_line(s, x + 2, y + 16, x + 4, y + 14, RGB(200, 120, 40));
        break;
    case T_BRUSH:
        gfx_thick_line(s, x + 6, y + 10, x + 14, y + 2, 1, RGB(120, 80, 40));
        gfx_fill_circle(s, x + 5, y + 12, 3, c);
        break;
    case T_ERASER:
        gfx_fill_rect(s, x + 3, y + 6, 12, 8, RGB(240, 150, 170));
        gfx_rect(s, x + 3, y + 6, 12, 8, c);
        gfx_fill_rect(s, x + 3, y + 10, 12, 4, 0xFFFFFF);
        break;
    case T_LINE: gfx_line(s, x + 2, y + 15, x + 15, y + 2, c); gfx_line(s, x + 3, y + 15, x + 15, y + 3, c); break;
    case T_RECT: gfx_rect(s, x + 2, y + 4, 14, 11, c); gfx_rect(s, x + 3, y + 5, 12, 9, c); break;
    case T_FILLRECT: gfx_fill_rect(s, x + 2, y + 4, 14, 11, c); break;
    case T_FILL:
        gfx_fill_rect(s, x + 3, y + 5, 9, 10, RGB(180, 180, 190));
        gfx_rect(s, x + 3, y + 5, 9, 10, c);
        gfx_fill_rect(s, x + 12, y + 8, 3, 7, RGB(0, 120, 215));
        break;
    case T_PICK:
        gfx_thick_line(s, x + 4, y + 14, x + 12, y + 6, 1, c);
        gfx_fill_circle(s, x + 13, y + 5, 3, c);
        break;
    }
}

static rect_t tool_rect(int t) { return rect_make(6 + (t % 2) * 27, TOOLBAR_H + 8 + (t / 2) * 27, 24, 24); }
static rect_t size_rect(int i) { return rect_make(8, TOOLBAR_H + 8 + 4 * 27 + 10 + i * 18, 48, 16); }
static rect_t palette_rect(window_t *w, int i)
{
    int y0 = w->surface->h - STATUS_H - PALETTE_H + 6;
    return rect_make(56 + (i % 15) * 20, y0 + (i / 15) * 19, 18, 17);
}

/* the colour of the shape being drawn; a transparent preview is shown grey */
static u32 draw_colour(struct paint *p, bool preview)
{
    u32 c = p->draw_button == MOUSE_RIGHT ? p->bg : p->fg;
    return preview && c == PIXEL_CLEAR ? 0x808080 : c;
}

/* draws a shape (or a stroke segment) onto surface s, offset by (dx,dy) */
static void draw_shape(struct paint *p, surface_t *s, int dx, int dy, int tool, int x0, int y0, int x1, int y1, u32 c)
{
    int r = sizes[p->size_idx] / 2;
    switch (tool) {
    case T_PENCIL: gfx_line(s, x0 + dx, y0 + dy, x1 + dx, y1 + dy, c); break;
    case T_BRUSH: gfx_thick_line(s, x0 + dx, y0 + dy, x1 + dx, y1 + dy, r, c); break;
    case T_ERASER: gfx_thick_line(s, x0 + dx, y0 + dy, x1 + dx, y1 + dy, r + 3, c); break;
    case T_LINE: gfx_thick_line(s, x0 + dx, y0 + dy, x1 + dx, y1 + dy, r, c); break;
    case T_RECT: case T_FILLRECT: {
        int x = MIN(x0, x1), y = MIN(y0, y1), wd = ABS(x1 - x0) + 1, ht = ABS(y1 - y0) + 1;
        if (tool == T_FILLRECT) gfx_fill_rect(s, x + dx, y + dy, wd, ht, c);
        else for (int k = 0; k <= r; k++) gfx_rect(s, x + dx + k, y + dy + k, wd - 2 * k, ht - 2 * k, c);
        break;
    }
    }
}

static void draw_status(window_t *w)
{
    struct paint *p = w->app;
    surface_t *s = w->surface;
    int sy = s->h - STATUS_H;
    gfx_fill_rect(s, 0, sy, s->w, STATUS_H, RGB(240, 240, 240));
    gfx_hline(s, 0, sy, s->w, RGB(215, 215, 220));
    char st[128];
    int n = snprintf(st, sizeof st, "%s   |   %dx%d px", tool_names[p->tool], p->canvas->w, p->canvas->h);
    if (p->mouse_in) snprintf(st + n, sizeof st - n, "   |   %d, %d", p->cx, p->cy);
    gfx_ui_text(s, 8, sy + 2, st, C_TEXT);
}

static void paint_status(window_t *w)
{
    draw_status(w);
    wm_invalidate_rect(w, rect_make(0, w->surface->h - STATUS_H, w->surface->w, STATUS_H));
}

static bool preview_active(struct paint *p)
{
    return p->drawing && (p->tool == T_LINE || p->tool == T_RECT || p->tool == T_FILLRECT);
}

static rect_t shape_bbox(struct paint *p, int x0, int y0, int x1, int y1)
{
    int r = sizes[p->size_idx] / 2 + 5;
    return rect_make(MIN(x0, x1) - r, MIN(y0, y1) - r, ABS(x1 - x0) + 2 * r + 1, ABS(y1 - y0) + 2 * r + 1);
}

/* Redraw only part of the canvas view (canvas coordinates): the canvas
 * pixels plus any shape preview, then invalidate just that area. */
static void refresh_region(window_t *w, rect_t cr)
{
    struct paint *p = w->app;
    surface_t *s = w->surface;
    rect_t v = view_rect(w);
    int cx0 = canvas_x(w), cy0 = canvas_y(w);
    rect_t sr = rect_intersect(rect_make(cr.x + cx0, cr.y + cy0, cr.w, cr.h),
                               rect_intersect(v, rect_make(cx0, cy0, p->canvas->w, p->canvas->h)));
    if (rect_empty(sr)) return;
    rect_t old = s->clip;
    gfx_set_clip(s, sr);
    draw_canvas(s, cx0, cy0, p->canvas);
    if (preview_active(p))
        draw_shape(p, s, cx0, cy0, p->tool, p->sx, p->sy, p->cx, p->cy, draw_colour(p, true));
    s->clip = old;
    wm_invalidate_rect(w, sr);
}

static void paint(window_t *w)
{
    struct paint *p = w->app;
    surface_t *s = w->surface;
    clamp_offset(w);
    /* toolbar */
    ui_toolbar(s, rect_make(0, 0, s->w, TOOLBAR_H));
    for (int i = 0; i < NBTN; i++) {
        gfx_fill_rect(s, p->btn[i].r.x, p->btn[i].r.y, p->btn[i].r.w, p->btn[i].r.h, RGB(240, 241, 245));
        ui_icon_button(s, p->btn[i].r, -1, p->btn[i].label, p->pressed == i ? BTN_PRESSED : p->hover == i ? BTN_HOVER : BTN_NORMAL);
    }
    /* toolbox */
    gfx_fill_rect(s, 0, TOOLBAR_H, TOOLBOX_W, s->h - TOOLBAR_H - STATUS_H, RGB(236, 238, 243));
    gfx_vline(s, TOOLBOX_W - 1, TOOLBAR_H, s->h - TOOLBAR_H - STATUS_H, RGB(205, 208, 216));
    for (int t = 0; t < NTOOLS; t++) {
        rect_t r = tool_rect(t);
        if (t == p->tool) { gfx_fill_rect(s, r.x, r.y, r.w, r.h, C_SELECT_LIGHT); gfx_rect(s, r.x, r.y, r.w, r.h, C_SELECT); }
        else { gfx_fill_rect(s, r.x, r.y, r.w, r.h, 0xFFFFFF); gfx_rect(s, r.x, r.y, r.w, r.h, RGB(200, 200, 208)); }
        tool_glyph(s, t, r.x + 3, r.y + 3, RGB(30, 30, 40));
    }
    for (int i = 0; i < 4; i++) {
        rect_t r = size_rect(i);
        if (i == p->size_idx) gfx_fill_rect(s, r.x, r.y, r.w, r.h, C_SELECT_LIGHT);
        int th = MAX(1, sizes[i] / 2 + (i > 0));
        gfx_fill_rect(s, r.x + 6, r.y + (r.h - th) / 2, r.w - 12, th, 0x000000);
    }
    /* canvas area */
    rect_t v = view_rect(w);
    gfx_fill_rect(s, v.x, v.y, v.w + SB, v.h + SB, RGB(171, 182, 200));
    rect_t old = s->clip;
    gfx_set_clip(s, v);
    int cx0 = canvas_x(w), cy0 = canvas_y(w);
    gfx_blend_rect(s, cx0 + 3, cy0 + 3, p->canvas->w, p->canvas->h, 0, 60);
    draw_canvas(s, cx0, cy0, p->canvas);
    if (p->drawing && (p->tool == T_LINE || p->tool == T_RECT || p->tool == T_FILLRECT)) {
        gfx_set_clip(s, rect_intersect(v, rect_make(cx0, cy0, p->canvas->w, p->canvas->h)));
        draw_shape(p, s, cx0, cy0, p->tool, p->sx, p->sy, p->cx, p->cy, draw_colour(p, true));
    }
    s->clip = old;
    /* scrollbars */
    ui_vscroll(s, rect_make(v.x + v.w, v.y, SB, v.h), p->canvas->h + 8, v.h, p->oy);
    rect_t hs = rect_make(v.x, v.y + v.h, v.w, SB);
    gfx_fill_rect(s, hs.x, hs.y, hs.w, hs.h, RGB(236, 236, 238));
    if (p->canvas->w + 8 > v.w) {
        int tw = MAX(20, hs.w * v.w / (p->canvas->w + 8));
        int tx = hs.x + (hs.w - tw) * p->ox / MAX(1, p->canvas->w + 8 - v.w);
        gfx_round_rect(s, tx, hs.y + 2, tw, hs.h - 4, 3, RGB(180, 180, 186));
    }
    /* palette */
    int py = s->h - STATUS_H - PALETTE_H;
    gfx_fill_rect(s, TOOLBOX_W, py, s->w - TOOLBOX_W, PALETTE_H, RGB(236, 238, 243));
    gfx_hline(s, TOOLBOX_W, py, s->w - TOOLBOX_W, RGB(205, 208, 216));
    draw_swatch(s, rect_make(20, py + 18, 22, 22), p->bg);
    gfx_rect(s, 20, py + 18, 22, 22, 0x404040);
    draw_swatch(s, rect_make(10, py + 8, 22, 22), p->fg);
    gfx_rect(s, 10, py + 8, 22, 22, 0x404040);
    for (int i = 0; i < NPAL; i++) {
        rect_t r = palette_rect(w, i);
        r.x += TOOLBOX_W - 50;
        draw_swatch(s, r, palette[i]);
        gfx_rect(s, r.x, r.y, r.w, r.h, RGB(120, 120, 130));
    }
    draw_status(w);
    w->mem_hint = p->canvas->w * p->canvas->h * 4;
    update_title(w);
    wm_invalidate(w);
}

/* scanline flood fill (OSDev-style span filling, explicit stack on the heap) */
static void flood_fill(surface_t *s, int x, int y, u32 c)
{
    u32 target = gfx_get_pixel(s, x, y);
    if (target == c) return;
    int cap = 4096, n = 0;
    int *stack = kmalloc(cap * 2 * sizeof(int));
    if (!stack) return;
    stack[n * 2] = x; stack[n * 2 + 1] = y; n++;
    while (n) {
        n--;
        int px = stack[n * 2], py = stack[n * 2 + 1];
        u32 *row = s->px + py * s->pitch;
        if (row[px] != target) continue;
        int l = px, r = px;
        while (l > 0 && row[l - 1] == target) l--;
        while (r < s->w - 1 && row[r + 1] == target) r++;
        for (int i = l; i <= r; i++) row[i] = c;
        for (int d = -1; d <= 1; d += 2) {
            int ny = py + d;
            if (ny < 0 || ny >= s->h) continue;
            u32 *nrow = s->px + ny * s->pitch;
            for (int i = l; i <= r; i++) {
                if (nrow[i] != target || (i > l && nrow[i - 1] == target)) continue;
                if (n == cap) {
                    int *ns = krealloc(stack, cap * 4 * sizeof(int));
                    if (!ns) { kfree(stack); return; }
                    stack = ns; cap *= 2;
                }
                stack[n * 2] = i; stack[n * 2 + 1] = ny; n++;
            }
        }
    }
    kfree(stack);
}

/* ---------------------------------------------------------------- files */
static void new_canvas(window_t *w, int cw, int ch)
{
    struct paint *p = w->app;
    surface_destroy(p->canvas);
    p->canvas = surface_create(cw, ch);
    gfx_fill_rect(p->canvas, 0, 0, cw, ch, 0xFFFFFF);
    p->ox = p->oy = 0;
    p->dirty = false;
}

/* Changes the canvas size.  The picture keeps its top-left corner; new space is white. */
static bool resize_canvas(window_t *w, int cw, int ch)
{
    struct paint *p = w->app;
    surface_t *n = surface_create(cw, ch);
    if (!n) return false;
    gfx_fill_rect(n, 0, 0, cw, ch, 0xFFFFFF);
    int ow = MIN(cw, p->canvas->w), oh = MIN(ch, p->canvas->h);
    for (int y = 0; y < oh; y++) memcpy(n->px + y * n->pitch, p->canvas->px + y * p->canvas->pitch, ow * sizeof(u32));
    surface_destroy(p->canvas);
    p->canvas = n;
    p->ox = p->oy = 0;
    p->dirty = true;
    klog("[pnt ] canvas size %dx%d\n", cw, ch);
    paint(w);
    return true;
}

static bool load_file(window_t *w, const char *path)
{
    struct paint *p = w->app;
    surface_t *img = bmp_load(path);
    if (!img) {
        msgbox(w, "Paint", "This file is not a supported BMP picture (24/32-bit uncompressed).", ICON_ERROR, MB_OK, NULL, NULL);
        return false;
    }
    surface_destroy(p->canvas);
    p->canvas = img;
    p->ox = p->oy = 0;
    p->dirty = false;
    strlcpy(p->path, path, sizeof p->path);
    klog("[pnt ] opened %s (%dx%d)\n", path, img->w, img->h);
    return true;
}

static void continue_after_save(window_t *w);

static bool save_to(window_t *w, const char *path)
{
    struct paint *p = w->app;
    int r = bmp_save_alpha(path, p->canvas);
    if (r) {
        char m[200];
        snprintf(m, sizeof m, "Cannot save \"%s\": %s.", path, nbfs_strerror(r));
        msgbox(w, "Paint", m, ICON_ERROR, MB_OK, NULL, NULL);
        return false;
    }
    strlcpy(p->path, path, sizeof p->path);
    p->dirty = false;
    klog("[pnt ] saved %s\n", path);
    paint(w);
    return true;
}

static void save_as_chosen(bool ok, const char *path, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    if (!ok) { ((struct paint *)w->app)->after_save = AFTER_NONE; return; }
    if (save_to(w, path)) continue_after_save(w);
}

static void save_as(window_t *w)
{
    struct paint *p = w->app;
    char dir[NBFS_PATH_MAX];
    if (p->path[0]) nbfs_path_parent(p->path, dir, sizeof dir);
    else strcpy(dir, "/Pictures");
    filedialog(w, FD_SAVE, "Save Picture As", dir, p->path[0] ? nbfs_path_basename(p->path) : "Untitled.bmp", ".bmp",
               save_as_chosen, w);
}

static void save(window_t *w)
{
    struct paint *p = w->app;
    if (p->path[0]) { if (save_to(w, p->path)) continue_after_save(w); }
    else save_as(w);
}

static void open_chosen(bool ok, const char *path, void *ctx)
{
    window_t *w = ctx;
    if (ok && wm_window_alive(w) && load_file(w, path)) paint(w);
}

static void continue_after_save(window_t *w)
{
    struct paint *p = w->app;
    int what = p->after_save;
    p->after_save = AFTER_NONE;
    if (what == AFTER_NEW) { new_canvas(w, 640, 400); p->path[0] = 0; paint(w); }
    else if (what == AFTER_OPEN) {
        char dir[NBFS_PATH_MAX];
        if (p->path[0]) nbfs_path_parent(p->path, dir, sizeof dir);
        else strcpy(dir, "/Pictures");
        filedialog(w, FD_OPEN, "Open Picture", dir, "", ".bmp", open_chosen, w);
    } else if (what == AFTER_CLOSE) wm_destroy_window(w);
}

static void prompt_cb(int r, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    struct paint *p = w->app;
    if (r == DLG_YES) save(w);
    else if (r == DLG_NO) { p->dirty = false; continue_after_save(w); }
    else p->after_save = AFTER_NONE;
}

static void guarded(window_t *w, int what)
{
    struct paint *p = w->app;
    p->after_save = what;
    if (!p->dirty) { continue_after_save(w); return; }
    char q[200];
    snprintf(q, sizeof q, "Do you want to save changes to %s?", p->path[0] ? nbfs_path_basename(p->path) : "Untitled");
    msgbox(w, "Paint", q, ICON_WARNING, MB_YESNOCANCEL, prompt_cb, w);
}

/* ---------------------------------------------------------------- input */
static void layout(window_t *w)
{
    struct paint *p = w->app;
    static const char *labels[NBTN] = { "New", "Open", "Save", "Save As", "Clear", "Size" };
    int x = 4;
    for (int i = 0; i < NBTN; i++) {
        int bw = 16 + gfx_text_width(labels[i]);
        p->btn[i] = (ui_btn_t){ rect_make(x, 3, bw, TOOLBAR_H - 6), i, labels[i], -1, false };
        x += bw + 4;
    }
}

/* ------------------------------------------------------------ canvas size */
#define SZ_W 380
#define SZ_H 250
#define SZ_FIELD rect_make(16, 190, 200, 26)

struct sizedlg {
    window_t *owner;
    int cur_w, cur_h;
    textfield_t tf;
    char lbl[NPRESET][16];
    ui_btn_t btn[NPRESET + 2];   /* presets, then Apply, Cancel */
    char err[80];
    int hover, pressed;
};

/* "800x600", "800 x 600", "800X600" or "800*600" */
static bool parse_size(const char *s, int *w, int *h)
{
    int a = 0, b = 0;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); if (a > 99999) return false; }
    while (*s == ' ') s++;
    if (*s != 'x' && *s != 'X' && *s != '*') return false;
    s++;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); if (b > 99999) return false; }
    while (*s == ' ') s++;
    if (*s) return false;
    *w = a; *h = b;
    return true;
}

static void sizedlg_paint(window_t *w)
{
    struct sizedlg *d = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    char t[64];
    snprintf(t, sizeof t, "Current size: %d x %d. Choose a preset:", d->cur_w, d->cur_h);
    gfx_ui_text(s, 16, 12, t, C_TEXT);
    for (int i = 0; i < NPRESET + 2; i++)
        ui_button(s, d->btn[i].r, d->btn[i].label,
                  d->pressed == i ? BTN_PRESSED : d->hover == i ? BTN_HOVER : BTN_NORMAL);
    gfx_ui_text(s, 16, 170, "Custom size (width x height):", C_TEXT);
    tf_draw(s, SZ_FIELD, &d->tf, true);
    if (d->err[0]) gfx_ui_text(s, 16, 224, d->err, RGB(200, 40, 40));
    wm_invalidate(w);
}

static void sizedlg_apply(window_t *w, struct sizedlg *d, int cw, int ch)
{
    if (!wm_window_alive(d->owner)) { wm_destroy_window(w); return; }
    if (cw < 1 || ch < 1 || cw > MAX_CANVAS_W || ch > MAX_CANVAS_H)
        snprintf(d->err, sizeof d->err, "Use 1 to %d wide and 1 to %d high.", MAX_CANVAS_W, MAX_CANVAS_H);
    else if (!resize_canvas(d->owner, cw, ch))
        strlcpy(d->err, "Not enough memory for that size.", sizeof d->err);
    else { wm_destroy_window(w); return; }   /* d is freed now: do not touch it */
    sizedlg_paint(w);
}

static void sizedlg_custom(window_t *w, struct sizedlg *d)
{
    int cw, ch;
    if (!parse_size(d->tf.text, &cw, &ch)) {
        strlcpy(d->err, "Type a size such as 800x600.", sizeof d->err);
        sizedlg_paint(w);
        return;
    }
    sizedlg_apply(w, d, cw, ch);
}

static void sizedlg_event(window_t *w, event_t *e)
{
    struct sizedlg *d = w->app;
    switch (e->type) {
    case EV_PAINT: sizedlg_paint(w); break;
    case EV_MOUSE_MOVE: {
        int h = ui_btn_hit(d->btn, NPRESET + 2, e->x, e->y);
        if (h != d->hover) { d->hover = h; sizedlg_paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        d->pressed = ui_btn_hit(d->btn, NPRESET + 2, e->x, e->y);
        if (rect_contains(SZ_FIELD, e->x, e->y)) tf_click(&d->tf, SZ_FIELD, e->x);
        sizedlg_paint(w);
        break;
    case EV_MOUSE_UP: {
        int p = d->pressed;
        d->pressed = -1;
        if (p >= 0 && ui_btn_hit(d->btn, NPRESET + 2, e->x, e->y) == p) {
            if (p == NPRESET + 1) { wm_destroy_window(w); return; }
            if (p == NPRESET) { sizedlg_custom(w, d); return; }
            sizedlg_apply(w, d, preset_w[p], preset_h[p]);
            return;
        }
        sizedlg_paint(w);
        break;
    }
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ENTER) { sizedlg_custom(w, d); return; }
        if (e->key.key == KEY_ESC) { wm_destroy_window(w); return; }
        if (tf_key(&d->tf, &e->key)) sizedlg_paint(w);
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(d); break;
    }
}

static void size_dialog(window_t *owner)
{
    struct paint *p = owner->app;
    struct sizedlg *d = kzalloc(sizeof *d);
    if (!d) return;
    d->owner = owner;
    d->cur_w = p->canvas->w;
    d->cur_h = p->canvas->h;
    d->hover = d->pressed = -1;
    char init[32];
    snprintf(init, sizeof init, "%d x %d", d->cur_w, d->cur_h);
    tf_set(&d->tf, init);
    for (int i = 0; i < NPRESET; i++) {
        snprintf(d->lbl[i], sizeof d->lbl[i], "%d x %d", preset_w[i], preset_h[i]);
        d->btn[i] = (ui_btn_t){ rect_make(16 + (i % 3) * 120, 36 + (i / 3) * 32, 112, 26), i, d->lbl[i], -1, false };
    }
    d->btn[NPRESET] = (ui_btn_t){ rect_make(226, 190, 64, 26), NPRESET, "Apply", -1, false };
    d->btn[NPRESET + 1] = (ui_btn_t){ rect_make(298, 190, 66, 26), NPRESET + 1, "Cancel", -1, false };
    window_t *win = wm_create("Canvas Size", ICON_PAINT, SZ_W, SZ_H, WF_DIALOG | WF_NO_TASKBAR, sizedlg_event, d);
    if (!win) { kfree(d); return; }
    wm_center(win);
    wm_set_modal(owner, win);
}

static void begin_stroke(window_t *w, int button)
{
    struct paint *p = w->app;
    u32 c = button == MOUSE_RIGHT ? p->bg : p->fg;
    p->sx = p->lx = p->cx;
    p->sy = p->ly = p->cy;
    if (p->tool == T_FILL) {
        if (p->cx >= 0 && p->cy >= 0 && p->cx < p->canvas->w && p->cy < p->canvas->h) {
            flood_fill(p->canvas, p->cx, p->cy, c);
            p->dirty = true;
        }
        return;
    }
    if (p->tool == T_PICK) {
        u32 picked = gfx_get_pixel(p->canvas, p->cx, p->cy);
        if (button == MOUSE_RIGHT) p->bg = picked; else p->fg = picked;
        return;
    }
    p->drawing = true;
    p->draw_button = button;
    if (p->tool == T_PENCIL || p->tool == T_BRUSH || p->tool == T_ERASER) {
        draw_shape(p, p->canvas, 0, 0, p->tool, p->cx, p->cy, p->cx, p->cy, p->tool == T_ERASER ? p->bg : c);
        p->dirty = true;
    }
}

static void on_event(window_t *w, event_t *ev)
{
    struct paint *p = w->app;
    rect_t v = view_rect(w);
    switch (ev->type) {
    case EV_PAINT: layout(w); paint(w); break;
    case EV_WHEEL:
        if (ev->key.mods & MOD_SHIFT) p->ox += ev->wheel * 48;
        else p->oy += ev->wheel * 48;
        paint(w);       /* paint() clamps the offsets */
        break;
    case EV_MOUSE_DOWN: {
        p->pressed = -1;
        if (ev->button == MOUSE_LEFT && (p->pressed = ui_btn_hit(p->btn, NBTN, ev->x, ev->y)) >= 0) { paint(w); break; }
        for (int t = 0; t < NTOOLS; t++) if (rect_contains(tool_rect(t), ev->x, ev->y)) { p->tool = t; klog("[pnt ] tool %s\n", tool_names[t]); }
        for (int i = 0; i < 4; i++) if (rect_contains(size_rect(i), ev->x, ev->y)) p->size_idx = i;
        for (int i = 0; i < NPAL; i++) {
            rect_t r = palette_rect(w, i);
            r.x += TOOLBOX_W - 50;
            if (rect_contains(r, ev->x, ev->y)) { if (ev->button == MOUSE_RIGHT) p->bg = palette[i]; else p->fg = palette[i]; }
        }
        if (rect_contains(rect_make(v.x + v.w, v.y, SB, v.h), ev->x, ev->y)) {
            p->drag_sb = 1;
            p->oy = ui_vscroll_offset_for(rect_make(v.x + v.w, v.y, SB, v.h), p->canvas->h + 8, v.h, ev->y);
        } else if (rect_contains(rect_make(v.x, v.y + v.h, v.w, SB), ev->x, ev->y)) {
            p->drag_sb = 2;
            p->ox = (ev->x - v.x) * (p->canvas->w + 8 - v.w) / MAX(1, v.w);
        } else if (rect_contains(v, ev->x, ev->y) && !p->drawing) {
            p->cx = ev->x - canvas_x(w);
            p->cy = ev->y - canvas_y(w);
            begin_stroke(w, ev->button);
        }
        paint(w);
        break;
    }
    case EV_MOUSE_MOVE: {
        int pcx = p->cx, pcy = p->cy;
        p->cx = ev->x - canvas_x(w);
        p->cy = ev->y - canvas_y(w);
        p->mouse_in = rect_contains(v, ev->x, ev->y);
        if (p->mouse_in) wm_set_app_cursor(CURSOR_CROSS);
        if (p->drag_sb) {
            if (p->drag_sb == 1) p->oy = ui_vscroll_offset_for(rect_make(v.x + v.w, v.y, SB, v.h), p->canvas->h + 8, v.h, ev->y);
            else p->ox = (ev->x - v.x) * (p->canvas->w + 8 - v.w) / MAX(1, v.w);
            paint(w);
            break;
        }
        if (p->drawing && (p->tool == T_PENCIL || p->tool == T_BRUSH || p->tool == T_ERASER)) {
            u32 c = p->tool == T_ERASER ? p->bg : (p->draw_button == MOUSE_RIGHT ? p->bg : p->fg);
            draw_shape(p, p->canvas, 0, 0, p->tool, p->lx, p->ly, p->cx, p->cy, c);
            refresh_region(w, shape_bbox(p, p->lx, p->ly, p->cx, p->cy));
            p->lx = p->cx; p->ly = p->cy;
        } else if (preview_active(p)) {
            refresh_region(w, rect_union(shape_bbox(p, p->sx, p->sy, pcx, pcy), shape_bbox(p, p->sx, p->sy, p->cx, p->cy)));
        } else if (!p->drawing) {
            int h = ui_btn_hit(p->btn, NBTN, ev->x, ev->y);
            if (h != p->hover) { p->hover = h; paint(w); break; }
        }
        paint_status(w);
        break;
    }
    case EV_MOUSE_UP: {
        p->drag_sb = 0;
        if (p->drawing && ev->button == p->draw_button) {
            bool shape = preview_active(p);
            if (shape)
                draw_shape(p, p->canvas, 0, 0, p->tool, p->sx, p->sy, p->cx, p->cy, p->draw_button == MOUSE_RIGHT ? p->bg : p->fg);
            p->drawing = false;
            bool was_dirty = p->dirty;
            p->dirty = true;
            if (shape) refresh_region(w, shape_bbox(p, p->sx, p->sy, p->cx, p->cy));
            if (was_dirty && p->pressed < 0) { paint_status(w); break; }   /* title already shows '*' */
        }
        int b = p->pressed;
        p->pressed = -1;
        if (b >= 0 && ui_btn_hit(p->btn, NBTN, ev->x, ev->y) == b) {
            if (b == B_NEW) guarded(w, AFTER_NEW);
            else if (b == B_OPEN) guarded(w, AFTER_OPEN);
            else if (b == B_SAVE) save(w);
            else if (b == B_SAVEAS) save_as(w);
            else if (b == B_SIZE) size_dialog(w);
            else { gfx_fill_rect(p->canvas, 0, 0, p->canvas->w, p->canvas->h, p->bg); p->dirty = true; klog("[pnt ] cleared\n"); }
        }
        paint(w);
        break;
    }
    case EV_KEY: {
        struct key_event *k = &ev->key;
        if (!k->pressed) break;
        if (k->mods & MOD_CTRL) {
            if (k->key == 's') { if (k->mods & MOD_SHIFT) save_as(w); else save(w); }
            else if (k->key == 'o') guarded(w, AFTER_OPEN);
            else if (k->key == 'n') guarded(w, AFTER_NEW);
            else if (k->key == 'w') size_dialog(w);
        } else if (k->key >= '1' && k->key <= '8') {
            p->tool = k->key - '1';
            paint(w);
        }
        break;
    }
    case EV_CLOSE: guarded(w, AFTER_CLOSE); break;
    case EV_DESTROY: surface_destroy(p->canvas); kfree(p); break;
    }
}

void paint_open(const char *path)
{
    struct paint *p = kzalloc(sizeof *p);
    if (!p) return;
    p->fg = 0x000000;
    p->bg = 0xFFFFFF;
    p->tool = T_PENCIL;
    p->size_idx = 1;
    p->hover = p->pressed = -1;
    p->canvas = surface_create(640, 400);
    gfx_fill_rect(p->canvas, 0, 0, 640, 400, 0xFFFFFF);
    window_t *w = wm_create("Untitled - Paint", ICON_PAINT, 740, 540, WF_RESIZABLE, on_event, p);
    if (!w) { surface_destroy(p->canvas); kfree(p); return; }
    w->min_w = 420;
    w->min_h = 300;
    if (path && path[0]) load_file(w, path);
    layout(w);
    paint(w);
}
