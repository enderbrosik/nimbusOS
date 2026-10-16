/*
 * NimbusOS - window manager and compositor.
 *
 * Rendering model
 *   Every window owns a client surface the application draws into.  The WM
 *   keeps a list of damaged screen rectangles.  render() recomposes only
 *   those rectangles into a full-screen back buffer: desktop (wallpaper +
 *   icons), then every window bottom-to-top (shadow, frame, title bar,
 *   client pixels), then the taskbar and popup menus.  All drawing is
 *   clipped to the damaged rectangle, and a window's client blit is bounded
 *   by its client size, so nothing can draw outside its bounds.  Finished
 *   rectangles are copied to the framebuffer (double buffering, no
 *   flicker).  The mouse cursor is drawn last, directly on the framebuffer,
 *   with the pixels underneath saved so it can be erased without trails.
 *
 * Input routing
 *   Mouse: popup menu -> taskbar -> windows top-to-bottom -> desktop.
 *   While a button is held inside a client area the window captures the
 *   mouse.  Keyboard: global shortcuts (Win, Alt+Tab, Alt+F4), then an open
 *   menu, then the focused window, else the desktop.
 */
#include "gui/wm.h"
#include "gui/theme.h"
#include "gui/menu.h"
#include "gui/icons.h"
#include "gui/desktop.h"
#include "gui/taskbar.h"
#include "drivers/fb.h"
#include "gui/bmp.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/task.h"
#include "kernel/panic.h"
#include "kernel/settings.h"
#include "fs/nbfs.h"

#define MAX_WINDOWS 48
#define MAX_DIRTY 48
int g_dblclick_ms = 450;

static window_t *zorder[MAX_WINDOWS];
static int nwin;
static window_t *focused;
static int next_id = 1;
static surface_t screen, *back;
/* UI scale: the desktop is drawn at 100/ui_pct of the real screen and each
 * pixel is then copied up (or down) to the framebuffer.  Everything (windows,
 * taskbar, text) is laid out in these "logical" pixels, so it looks bigger
 * or smaller without any app changing. */
static int ui_pct = 100;
static int phys_w, phys_h;          /* the real screen (framebuffer) size */
static int *map_x, *map_y;          /* physical pixel -> logical pixel in `back` */
static rect_t dirty[MAX_DIRTY];
static int ndirty;
static int cascade;

/* mouse state */
static int mx, my;
static u8 mbuttons;
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CAPTURE, DRAG_TITLE_BTN, DRAG_DESKTOP, DRAG_TASKBAR, DRAG_DND };
static char dnd_path[NBFS_PATH_MAX];
static int dnd_icon;

static int drag_mode;
static window_t *drag_win;
static int drag_dx, drag_dy, drag_edges, drag_btn, press_x, press_y;
static rect_t drag_frame;
static u64 last_click_t;
static int last_click_x, last_click_y, last_click_btn;
static int hover_btn;               /* title button under the mouse (HT_*) */
static window_t *hover_win;

/* cursor */
static int app_cursor = CURSOR_ARROW, cur_cursor = CURSOR_ARROW;
static int cursor_x = -100, cursor_y = -100, cursor_drawn_shape;
static bool cursor_visible;
#define CSAVE 96                          /* largest cursor (32 px) at 3x */
#define CUR_IMG_MAX 32
#define CUR_KEY 0xFF00FF                  /* magenta pixels of an image cursor are see-through */
static surface_t *cur_img;                 /* custom pointer image, NULL if none */
static u32 cursor_save[CSAVE * CSAVE];
static int ptr_style, cur_scale = 1;
static u32 cur_fill = 0xFFFFFF;
static rect_t cursor_saved_rect;

enum { HT_NONE, HT_CLIENT, HT_TITLE, HT_FRAME, HT_BTN_CLOSE, HT_BTN_MAX, HT_BTN_MIN, HT_RESIZE };
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8

/* ---------------------------------------------------------------------
 * cursors: 'X' = black outline, '#' = white fill; hot spot listed
 * ------------------------------------------------------------------- */
struct cursor_def { int w, h, hx, hy; const char *rows[24]; };
static const struct cursor_def cursors[] = {
    [CURSOR_ARROW] = { 12, 19, 0, 0, {
        "X...........", "XX..........", "X#X.........", "X##X........", "X###X.......", "X####X......",
        "X#####X.....", "X######X....", "X#######X...", "X########X..", "X#########X.", "X######XXXXX",
        "X###X##X....", "X##X.X##X...", "X#X..X##X...", "XX....X##X..", "X.....X##X..", ".......X##X.",
        "........XX.." } },
    [CURSOR_TEXT] = { 9, 18, 4, 9, {
        "XXXX.XXXX", "X###X###X", "XXXX#XXXX", "...X#X...", "...X#X...", "...X#X...", "...X#X...",
        "...X#X...", "...X#X...", "...X#X...", "...X#X...", "...X#X...", "...X#X...", "...X#X...",
        "XXXX#XXXX", "X###X###X", "XXXX.XXXX", "........." } },
    [CURSOR_RESIZE_H] = { 21, 9, 10, 4, {
        "....X...........X....", "...XX...........XX...", "..X#XXXXXXXXXXXXX#X..", ".X###############X#X.",
        "X#################X#X", ".X###############X#X.", "..X#XXXXXXXXXXXXX#X..", "...XX...........XX...",
        "....X...........X...." } },
    [CURSOR_RESIZE_V] = { 9, 21, 4, 10, {
        "....X....", "...X#X...", "..X###X..", ".X#####X.", "XXXX#XXXX", "...X#X...", "...X#X...",
        "...X#X...", "...X#X...", "...X#X...", "...X#X...", "...X#X...", "...X#X...", "...X#X...",
        "...X#X...", "...X#X...", "XXXX#XXXX", ".X#####X.", "..X###X..", "...X#X...", "....X...." } },
    [CURSOR_RESIZE_NWSE] = { 15, 15, 7, 7, {
        "XXXXXXX........", "X#####X........", "X####X.........", "X###X#X........", "X##X.X#X.......",
        "X#X...X#X......", "XX.....X#X.....", ".......X#X.....", "......X#X.....X", ".......X#X...XX",
        "........X#X.X#X", ".........X#X##X", "........X####X.", "........X#####X", "........XXXXXXX" } },
    [CURSOR_RESIZE_NESW] = { 15, 15, 7, 7, {
        "........XXXXXXX", "........X#####X", ".........X####X", "........X#X###X", ".......X#X.X##X",
        "......X#X...X#X", ".....X#X.....XX", ".....X#X.......", "X.....X#X......", "XX...X#X.......",
        "X#X.X#X........", "X##X#X.........", ".X####X........", "X#####X........", "XXXXXXX........" } },
    [CURSOR_DOT] = { 9, 9, 4, 4, {
        "..XXXXX..", ".X#####X.", "X#######X", "X#######X", "X#######X", "X#######X", "X#######X", ".X#####X.", "..XXXXX.." } },
    [CURSOR_RING] = { 11, 11, 5, 5, {
        "...XXXXX...", "..X#####X..", ".X#XXXXX#X.", "X#X.....X#X", "X#X.....X#X", "X#X.....X#X", "X#X.....X#X",
        "X#X.....X#X", ".X#XXXXX#X.", "..X#####X..", "...XXXXX..." } },
    [CURSOR_PLUS] = { 11, 11, 5, 5, {
        "....XXX....", "....X#X....", "....X#X....", "....X#X....", "XXXX###XXXX", "X#########X",
        "XXXX###XXXX", "....X#X....", "....X#X....", "....X#X....", "....XXX...." } },
    [CURSOR_CROSS] = { 15, 15, 7, 7, {
        "......XXX......", "......X#X......", "......X#X......", "......X#X......", "......X#X......",
        "......X#X......", "XXXXXXX#XXXXXXX", "X######.######X", "XXXXXXX#XXXXXXX", "......X#X......",
        "......X#X......", "......X#X......", "......X#X......", "......X#X......", "......XXX......" } },
};

/* The shape drawn for the current pointer (arrow is replaced by the chosen style). */
static int pointer_shape(void)
{
    if (cur_cursor != CURSOR_ARROW) return cur_cursor;
    static const int styles[4] = { CURSOR_ARROW, CURSOR_DOT, CURSOR_RING, CURSOR_PLUS };
    return styles[ptr_style & 3];
}

static bool custom_active(void) { return ptr_style == 4 && cur_img && cur_cursor == CURSOR_ARROW; }

/* Size and hot spot of the pointer that is drawn now, at the current scale. */
static void cursor_geom(int *w, int *h, int *hx, int *hy)
{
    if (custom_active()) { *w = cur_img->w * cur_scale; *h = cur_img->h * cur_scale; *hx = 0; *hy = 0; return; }
    const struct cursor_def *c = &cursors[pointer_shape()];
    *w = c->w * cur_scale; *h = c->h * cur_scale; *hx = c->hx * cur_scale; *hy = c->hy * cur_scale;
}

static rect_t cursor_rect(int x, int y)
{
    int w, h, hx, hy;
    cursor_geom(&w, &h, &hx, &hy);
    return rect_intersect(rect_make(x - hx, y - hy, w, h), rect_make(0, 0, screen.w, screen.h));
}

static void cursor_hide(void)
{
    if (!cursor_visible) return;
    rect_t r = cursor_saved_rect;
    for (int j = 0; j < r.h; j++)
        memcpy(screen.px + (r.y + j) * screen.pitch + r.x, cursor_save + j * CSAVE, r.w * 4);
    cursor_visible = false;
}

static void cursor_show(void)
{
    int px = mx * phys_w / SCREEN_W, py = my * phys_h / SCREEN_H;
    int w, h, hx, hy;
    cursor_geom(&w, &h, &hx, &hy);
    rect_t r = cursor_rect(px, py);
    for (int j = 0; j < r.h; j++)            /* save what is underneath */
        memcpy(cursor_save + j * CSAVE, screen.px + (r.y + j) * screen.pitch + r.x, r.w * 4);
    cursor_saved_rect = r;
    int s = cur_scale;
    int ox = px - hx, oy = py - hy;
    if (custom_active()) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int x = ox + i, y = oy + j;
                u32 c = cur_img->px[(j / s) * cur_img->pitch + i / s];
                if (x < 0 || y < 0 || x >= screen.w || y >= screen.h || c == CUR_KEY) continue;
                screen.px[y * screen.pitch + x] = c;
            }
    } else {
        const struct cursor_def *c = &cursors[pointer_shape()];
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                char ch = c->rows[j / s][i / s];
                int x = ox + i, y = oy + j;
                if (x < 0 || y < 0 || x >= screen.w || y >= screen.h) continue;
                if (ch == 'X') screen.px[y * screen.pitch + x] = 0x000000;
                else if (ch == '#') screen.px[y * screen.pitch + x] = cur_fill;
            }
    }
    cursor_x = mx; cursor_y = my; cursor_drawn_shape = cur_cursor;
    cursor_visible = true;
}

/* ---------------------------------------------------------------------
 * damage tracking
 * ------------------------------------------------------------------- */
void wm_damage(rect_t r)
{
    r = rect_intersect(r, rect_make(0, 0, SCREEN_W, SCREEN_H));
    if (rect_empty(r)) return;
    /* merge with an overlapping rectangle when that does not waste much */
    for (int i = 0; i < ndirty; i++) {
        rect_t u = rect_union(dirty[i], r);
        int waste = u.w * u.h - dirty[i].w * dirty[i].h - r.w * r.h;
        if (rect_overlaps(dirty[i], r) || waste < 4096) {
            dirty[i] = u;
            return;
        }
    }
    if (ndirty == MAX_DIRTY) {
        for (int i = 1; i < ndirty; i++) dirty[0] = rect_union(dirty[0], dirty[i]);
        ndirty = 1;
        dirty[0] = rect_union(dirty[0], r);
        return;
    }
    dirty[ndirty++] = r;
}

static rect_t window_bounds(window_t *w)
{
    return rect_make(w->frame.x, w->frame.y, w->frame.w + SHADOW, w->frame.h + SHADOW);
}

rect_t wm_client_rect(window_t *w)
{
    return rect_make(w->frame.x + BORDER, w->frame.y + BORDER + TITLE_H,
                     w->frame.w - 2 * BORDER, w->frame.h - 2 * BORDER - TITLE_H);
}

void wm_invalidate(window_t *w) { if (!w->minimized) wm_damage(wm_client_rect(w)); }

void wm_invalidate_rect(window_t *w, rect_t r)
{
    if (w->minimized) return;
    rect_t c = wm_client_rect(w);
    r.x += c.x; r.y += c.y;
    wm_damage(rect_intersect(r, c));
}

/* ---------------------------------------------------------------------
 * drawing windows
 * ------------------------------------------------------------------- */
static rect_t title_btn_rect(window_t *w, int which)
{
    rect_t f = w->frame;
    int bw = 30, bh = TITLE_H - 4;
    int x = f.x + f.w - BORDER - bw - 2, y = f.y + BORDER + 2;
    if (which == HT_BTN_CLOSE) return rect_make(x, y, bw, bh);
    if (w->flags & WF_DIALOG) return rect_make(0, 0, 0, 0);
    if (which == HT_BTN_MAX) {
        if (w->flags & WF_NO_MAXIMIZE || !(w->flags & WF_RESIZABLE)) return rect_make(0, 0, 0, 0);
        return rect_make(x - bw - 2, y, bw, bh);
    }
    if (which == HT_BTN_MIN) {
        int n = (w->flags & WF_NO_MAXIMIZE || !(w->flags & WF_RESIZABLE)) ? 1 : 2;
        return rect_make(x - n * (bw + 2), y, bw, bh);
    }
    return rect_make(0, 0, 0, 0);
}

static void draw_title_btn(surface_t *s, window_t *w, int which, bool active)
{
    rect_t r = title_btn_rect(w, which);
    if (rect_empty(r)) return;
    bool hover = hover_win == w && hover_btn == which;
    bool pressed = drag_mode == DRAG_TITLE_BTN && drag_win == w && drag_btn == which && hover;
    if (hover || pressed) {
        u32 c = which == HT_BTN_CLOSE ? RGB(220, 50, 50) : (active ? RGB(70, 140, 220) : RGB(175, 180, 192));
        if (pressed) c = color_darken(c, 40);
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 3, c);
    }
    u32 fg = 0xFFFFFF;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (which == HT_BTN_CLOSE) {
        gfx_line(s, cx - 4, cy - 4, cx + 4, cy + 4, fg); gfx_line(s, cx - 3, cy - 4, cx + 4, cy + 3, fg);
        gfx_line(s, cx + 4, cy - 4, cx - 4, cy + 4, fg); gfx_line(s, cx + 3, cy - 4, cx - 4, cy + 3, fg);
    } else if (which == HT_BTN_MAX) {
        if (w->maximized) {
            gfx_rect(s, cx - 2, cy - 5, 8, 7, fg);
            gfx_fill_rect(s, cx - 5, cy - 2, 8, 7, active ? C_TITLE_ACTIVE_2 : C_TITLE_INACTIVE_2);
            gfx_rect(s, cx - 5, cy - 2, 8, 7, fg);
            gfx_hline(s, cx - 5, cy - 1, 8, fg);
        } else {
            gfx_rect(s, cx - 5, cy - 5, 11, 10, fg);
            gfx_hline(s, cx - 5, cy - 4, 11, fg);
        }
    } else if (which == HT_BTN_MIN) {
        gfx_fill_rect(s, cx - 5, cy + 3, 11, 2, fg);
    }
}

static void draw_window(surface_t *s, window_t *w)
{
    rect_t f = w->frame;
    bool active = w == focused;
    /* soft drop shadow on the right and bottom */
    for (int d = 0; d < SHADOW; d++) {
        int a = 70 * (SHADOW - d) / SHADOW;
        gfx_blend_rect(s, f.x + f.w + d, f.y + 4 + d, 1, f.h - 4, 0, a);
        gfx_blend_rect(s, f.x + 4 + d, f.y + f.h + d, f.w - 4, 1, 0, a);
    }
    gfx_rect(s, f.x, f.y, f.w, f.h, active ? C_BORDER_ACTIVE : C_BORDER_INACTIVE);
    gfx_gradient_v(s, f.x + BORDER, f.y + BORDER, f.w - 2 * BORDER, TITLE_H,
                   active ? C_TITLE_ACTIVE_2 : C_TITLE_INACTIVE_2, active ? C_TITLE_ACTIVE_1 : C_TITLE_INACTIVE_1);
    icon_draw_small(s, w->icon, f.x + BORDER + 6, f.y + BORDER + (TITLE_H - 16) / 2);
    rect_t minb = title_btn_rect(w, HT_BTN_MIN), closeb = title_btn_rect(w, HT_BTN_CLOSE);
    int text_max = (rect_empty(minb) ? closeb.x : minb.x) - (f.x + 30) - 4;
    rect_t old = s->clip;
    gfx_set_clip(s, rect_intersect(old, rect_make(f.x + 28, f.y, text_max + 2, TITLE_H + 2)));
    int ty = f.y + BORDER + (TITLE_H - 16) / 2;
    gfx_set_clip(s, rect_intersect(s->clip, rect_make(f.x + 28, f.y, text_max + 2, TITLE_H + 2)));
    char t[80];
    strlcpy(t, w->title, sizeof t);
    /* bold title with a soft shadow; ellipsize to the space left of the buttons */
    if (gfx_ui_width(t) + 1 > text_max) {
        int n = strlen(t);
        while (n > 0 && gfx_ui_width_n(t, n) + gfx_ui_width("...") + 1 > text_max) n--;
        strcpy(t + n, "...");
    }
    gfx_ui_text_bold(s, f.x + 29, ty + 1, t, active ? color_darken(C_TITLE_ACTIVE_1, 80) : RGB(90, 94, 104));
    gfx_ui_text_bold(s, f.x + 28, ty, t, 0xFFFFFF);
    s->clip = old;
    draw_title_btn(s, w, HT_BTN_MIN, active);
    draw_title_btn(s, w, HT_BTN_MAX, active);
    draw_title_btn(s, w, HT_BTN_CLOSE, active);
    rect_t c = wm_client_rect(w);
    gfx_blit(s, c.x, c.y, w->surface, 0, 0, c.w, c.h);
}

static void compose(rect_t r)
{
    gfx_set_clip(back, r);
    desktop_draw(back);
    for (int i = 0; i < nwin; i++) {
        window_t *w = zorder[i];
        if (w->minimized || w->destroyed) continue;
        if (rect_overlaps(window_bounds(w), r)) draw_window(back, w);
    }
    desktop_draw_overlay(back);
    if (drag_mode == DRAG_DND) {
        /* ghost: icon + name, translucent, beside the cursor */
        const char *name = nbfs_path_basename(dnd_path);
        int gx = mx + 10, gy = my + 12, gw = gfx_ui_width(name) + 44;
        if (rect_overlaps(rect_make(gx, gy, gw, 36), r)) {
            gfx_blend_rect(back, gx, gy, gw, 36, C_SELECT, 110);
            icon_draw(back, dnd_icon, gx + 2, gy + 2);
            gfx_ui_text(back, gx + 38, gy + 10, name, 0xFFFFFF);
        }
    }
    taskbar_draw(back);
    menu_draw(back);
    gfx_reset_clip(back);
}

/* Copy the logical rectangle lr of `back` to the screen (scaled). */
static void present(rect_t lr)
{
    if (ui_pct == 100) { gfx_blit(&screen, lr.x, lr.y, back, lr.x, lr.y, lr.w, lr.h); return; }
    int x0 = lr.x * phys_w / SCREEN_W, y0 = lr.y * phys_h / SCREEN_H;
    int x1 = MIN(((lr.x + lr.w) * phys_w + SCREEN_W - 1) / SCREEN_W, phys_w);
    int y1 = MIN(((lr.y + lr.h) * phys_h + SCREEN_H - 1) / SCREEN_H, phys_h);
    for (int y = y0; y < y1; y++) {
        const u32 *src = back->px + map_y[y] * back->pitch;
        u32 *dst = screen.px + y * screen.pitch;
        for (int x = x0; x < x1; x++) dst[x] = src[map_x[x]];
    }
}

static void render(void)
{
    bool cursor_moved = mx != cursor_x || my != cursor_y || cur_cursor != cursor_drawn_shape;
    if (!ndirty && !cursor_moved && cursor_visible) return;
    for (int i = 0; i < ndirty; i++) compose(dirty[i]);
    cursor_hide();
    for (int i = 0; i < ndirty; i++) present(dirty[i]);
    ndirty = 0;
    cursor_show();
}

/* ---------------------------------------------------------------------
 * window list management
 * ------------------------------------------------------------------- */
static int zindex(window_t *w)
{
    for (int i = 0; i < nwin; i++) if (zorder[i] == w) return i;
    return -1;
}

int wm_window_count(void) { return nwin; }
window_t *wm_window_at(int z) { return z >= 0 && z < nwin ? zorder[z] : NULL; }
bool wm_window_alive(window_t *w) { return w && zindex(w) >= 0 && !w->destroyed; }
window_t *wm_focused(void) { return focused; }
int wm_mouse_x(void) { return mx; }
int wm_mouse_y(void) { return my; }
u64 wm_ticks(void) { return timer_ticks(); }
void wm_set_app_cursor(int c) { app_cursor = c; }

static void send(window_t *w, event_t *e)
{
    if (w && !w->destroyed && w->handler) w->handler(w, e);
}

static void send_simple(window_t *w, int type)
{
    event_t e;
    memset(&e, 0, sizeof e);
    e.type = type;
    send(w, &e);
}

static void top_visible_focus(void)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (!zorder[i]->minimized && !zorder[i]->destroyed) { wm_focus(zorder[i]); return; }
    window_t *old = focused;
    focused = NULL;
    if (old) send_simple(old, EV_BLUR);
    taskbar_changed();
}

void wm_focus(window_t *w)
{
    if (!w) { top_visible_focus(); return; }
    while (w->modal && wm_window_alive(w->modal)) w = w->modal;
    if (w->minimized) wm_restore(w);
    int i = zindex(w);
    if (i < 0) return;
    if (i != nwin - 1) {                    /* raise to the top */
        memmove(&zorder[i], &zorder[i + 1], (nwin - 1 - i) * sizeof zorder[0]);
        zorder[nwin - 1] = w;
        wm_damage(window_bounds(w));
    }
    if (focused != w) {
        window_t *old = focused;
        focused = w;
        if (old && wm_window_alive(old)) { wm_damage(window_bounds(old)); send_simple(old, EV_BLUR); }
        wm_damage(window_bounds(w));
        send_simple(w, EV_FOCUS);
        taskbar_changed();
    }
}

window_t *wm_create(const char *title, int icon, int cw, int ch, int flags, wm_handler_t handler, void *app)
{
    if (nwin >= MAX_WINDOWS) return NULL;
    window_t *w = kzalloc(sizeof *w);
    if (!w) return NULL;
    if (cw > SCREEN_W - 2 * BORDER) cw = SCREEN_W - 2 * BORDER;
    if (ch > SCREEN_H - TASKBAR_H - TITLE_H - 2 * BORDER) ch = SCREEN_H - TASKBAR_H - TITLE_H - 2 * BORDER;
    w->surface = surface_create(cw, ch);
    if (!w->surface) { kfree(w); return NULL; }
    gfx_fill_rect(w->surface, 0, 0, cw, ch, C_WINDOW_BG);
    w->id = next_id++;
    strlcpy(w->title, title, sizeof w->title);
    w->icon = icon;
    w->flags = flags;
    w->handler = handler;
    w->app = app;
    w->min_w = 160;
    w->min_h = 60;
    int fw = cw + 2 * BORDER, fh = ch + 2 * BORDER + TITLE_H;
    int x = 80 + cascade * 28, y = 50 + cascade * 28;
    cascade = (cascade + 1) % 8;
    if (x + fw > SCREEN_W) x = MAX(0, SCREEN_W - fw);
    if (y + fh > SCREEN_H - TASKBAR_H) y = MAX(0, SCREEN_H - TASKBAR_H - fh);
    w->frame = rect_make(x, y, fw, fh);
    w->restore = w->frame;
    zorder[nwin++] = w;
    klog("[wm  ] window %d created: \"%s\" %dx%d\n", w->id, w->title, cw, ch);
    send_simple(w, EV_PAINT);
    wm_focus(w);
    wm_damage(window_bounds(w));
    taskbar_changed();
    return w;
}

void wm_destroy_window(window_t *w)
{
    if (!w || w->destroyed) return;
    w->destroyed = true;
    klog("[wm  ] window %d destroyed: \"%s\"\n", w->id, w->title);
    /* destroy dialogs owned by this window */
    for (int i = 0; i < nwin; i++)
        if (zorder[i]->owner == w) wm_destroy_window(zorder[i]);
    if (w->owner && w->owner->modal == w) w->owner->modal = NULL;
    send_simple(w, EV_DESTROY);
    w->handler = NULL;
    wm_damage(window_bounds(w));
    if (drag_win == w) { drag_win = NULL; drag_mode = DRAG_NONE; }
    if (hover_win == w) hover_win = NULL;
    if (focused == w) {
        focused = NULL;
        if (w->owner && wm_window_alive(w->owner)) wm_focus(w->owner);
        else top_visible_focus();
    }
    taskbar_changed();
}

static void reap(void)
{
    for (int i = 0; i < nwin; i++) {
        window_t *w = zorder[i];
        if (!w->destroyed) continue;
        memmove(&zorder[i], &zorder[i + 1], (nwin - 1 - i) * sizeof zorder[0]);
        nwin--;
        i--;
        surface_destroy(w->surface);
        kfree(w);
    }
}

void wm_close(window_t *w)
{
    if (!w || w->destroyed) return;
    if (w->modal && wm_window_alive(w->modal)) { wm_focus(w->modal); return; }
    event_t e;
    memset(&e, 0, sizeof e);
    e.type = EV_CLOSE;
    if (w->handler) w->handler(w, &e);
    else wm_destroy_window(w);
}

void wm_set_modal(window_t *owner, window_t *dialog)
{
    if (!owner || !dialog) return;
    owner->modal = dialog;
    dialog->owner = owner;
    wm_focus(dialog);
}

void wm_set_title(window_t *w, const char *t)
{
    strlcpy(w->title, t, sizeof w->title);
    wm_damage(rect_make(w->frame.x, w->frame.y, w->frame.w, TITLE_H + BORDER));
    taskbar_changed();
}

void wm_set_timer(window_t *w, u32 ms)
{
    w->timer_ms = ms;
    w->timer_next = timer_ticks() + ms;
}

void wm_set_frame(window_t *w, rect_t f)
{
    int minfw = w->min_w + 2 * BORDER, minfh = w->min_h + 2 * BORDER + TITLE_H;
    if (f.w < minfw) f.w = minfw;
    if (f.h < minfh) f.h = minfh;
    if (f.w > SCREEN_W) f.w = SCREEN_W;
    if (f.h > SCREEN_H - TASKBAR_H) f.h = SCREEN_H - TASKBAR_H;
    wm_damage(window_bounds(w));
    bool resized = f.w != w->frame.w || f.h != w->frame.h;
    w->frame = f;
    if (resized) {
        rect_t c = wm_client_rect(w);
        surface_t *ns = surface_create(c.w, c.h);
        if (ns) {
            gfx_fill_rect(ns, 0, 0, c.w, c.h, C_WINDOW_BG);
            surface_destroy(w->surface);
            w->surface = ns;
        }
        send_simple(w, EV_RESIZE);
        send_simple(w, EV_PAINT);
    }
    wm_damage(window_bounds(w));
}

void wm_move(window_t *w, int x, int y)
{
    wm_set_frame(w, rect_make(x, y, w->frame.w, w->frame.h));
}

void wm_center(window_t *w)
{
    wm_move(w, (SCREEN_W - w->frame.w) / 2, (SCREEN_H - TASKBAR_H - w->frame.h) / 2);
}

void wm_minimize(window_t *w)
{
    if (w->minimized) return;
    klog("[wm  ] window %d minimized\n", w->id);
    w->minimized = true;
    wm_damage(window_bounds(w));
    if (focused == w) top_visible_focus();
    taskbar_changed();
}

void wm_restore(window_t *w)
{
    if (!w->minimized) return;
    klog("[wm  ] window %d restored\n", w->id);
    w->minimized = false;
    wm_damage(window_bounds(w));
    taskbar_changed();
}

void wm_toggle_maximize(window_t *w)
{
    if (!(w->flags & WF_RESIZABLE) || (w->flags & WF_NO_MAXIMIZE)) return;
    if (w->maximized) {
        w->maximized = false;
        wm_set_frame(w, w->restore);
    } else {
        w->restore = w->frame;
        w->maximized = true;
        wm_set_frame(w, rect_make(0, 0, SCREEN_W, SCREEN_H - TASKBAR_H));
    }
}

/* ---------------------------------------------------------------------
 * hit testing
 * ------------------------------------------------------------------- */
static int hit_window(window_t *w, int x, int y, int *edges)
{
    rect_t f = w->frame;
    bool resizable = (w->flags & WF_RESIZABLE) && !w->maximized;
    rect_t grab = resizable ? rect_make(f.x - RESIZE_OUT, f.y - RESIZE_OUT, f.w + 2 * RESIZE_OUT, f.h + 2 * RESIZE_OUT) : f;
    if (!rect_contains(grab, x, y)) return HT_NONE;
    if (resizable) {
        int e = 0;
        if (x < f.x + RESIZE_IN) e |= EDGE_L;
        if (x >= f.x + f.w - RESIZE_IN) e |= EDGE_R;
        if (y < f.y + RESIZE_IN) e |= EDGE_T;
        if (y >= f.y + f.h - RESIZE_IN) e |= EDGE_B;
        if (e) {
            /* generous corners: 14 px along each edge */
            if (e & (EDGE_L | EDGE_R)) {
                if (y < f.y + 14) e |= EDGE_T;
                if (y >= f.y + f.h - 14) e |= EDGE_B;
            }
            if (e & (EDGE_T | EDGE_B)) {
                if (x < f.x + 14) e |= EDGE_L;
                if (x >= f.x + f.w - 14) e |= EDGE_R;
            }
            *edges = e;
            return HT_RESIZE;
        }
    }
    if (rect_contains(wm_client_rect(w), x, y)) return HT_CLIENT;
    if (y < f.y + BORDER + TITLE_H) {
        if (rect_contains(title_btn_rect(w, HT_BTN_CLOSE), x, y)) return HT_BTN_CLOSE;
        if (rect_contains(title_btn_rect(w, HT_BTN_MAX), x, y)) return HT_BTN_MAX;
        if (rect_contains(title_btn_rect(w, HT_BTN_MIN), x, y)) return HT_BTN_MIN;
        return HT_TITLE;
    }
    return HT_FRAME;
}

static window_t *window_at(int x, int y, int *part, int *edges)
{
    for (int i = nwin - 1; i >= 0; i--) {
        window_t *w = zorder[i];
        if (w->minimized || w->destroyed) continue;
        int p = hit_window(w, x, y, edges);
        if (p != HT_NONE) { *part = p; return w; }
    }
    return NULL;
}

static int edges_cursor(int e)
{
    if ((e & EDGE_L && e & EDGE_T) || (e & EDGE_R && e & EDGE_B)) return CURSOR_RESIZE_NWSE;
    if ((e & EDGE_R && e & EDGE_T) || (e & EDGE_L && e & EDGE_B)) return CURSOR_RESIZE_NESW;
    if (e & (EDGE_L | EDGE_R)) return CURSOR_RESIZE_H;
    return CURSOR_RESIZE_V;
}

static void mouse_event_to(window_t *w, int type, int button)
{
    event_t e;
    memset(&e, 0, sizeof e);
    rect_t c = wm_client_rect(w);
    e.type = type;
    e.x = mx - c.x;
    e.y = my - c.y;
    e.button = button;
    e.buttons = mbuttons;
    send(w, &e);
}

static void set_hover_btn(window_t *w, int part)
{
    int b = (part == HT_BTN_CLOSE || part == HT_BTN_MAX || part == HT_BTN_MIN) ? part : 0;
    if (w != hover_win || b != hover_btn) {
        if (hover_win && wm_window_alive(hover_win)) wm_damage(window_bounds(hover_win));
        hover_win = b ? w : NULL;
        hover_btn = b;
        if (w) wm_damage(window_bounds(w));
    }
}

/* ---------------------------------------------------------------------
 * mouse handling
 * ------------------------------------------------------------------- */
static rect_t dnd_rect(void)
{
    return rect_make(mx + 10, my + 12, gfx_ui_width(nbfs_path_basename(dnd_path)) + 46, 38);
}
static rect_t last_dnd;

void wm_begin_dnd(const char *path, int icon)
{
    strlcpy(dnd_path, path, sizeof dnd_path);
    dnd_icon = icon;
    drag_mode = DRAG_DND;
    last_dnd = dnd_rect();
    wm_damage(last_dnd);
    klog("[wm  ] drag started: %s\n", path);
}

window_t *wm_window_under(int x, int y)
{
    int part = HT_NONE, edges;
    if (rect_contains(taskbar_rect(), x, y)) return NULL;
    return window_at(x, y, &part, &edges);
}

void wm_send_drop(window_t *w, const char *path, int sx, int sy, bool copy)
{
    if (!wm_window_alive(w)) return;
    if (w->modal && wm_window_alive(w->modal)) return;
    event_t e;
    memset(&e, 0, sizeof e);
    rect_t c = wm_client_rect(w);
    e.type = EV_DROP;
    e.x = sx - c.x;
    e.y = sy - c.y;
    e.path = path;
    e.copy = copy;
    klog("[wm  ] drop %s on \"%s\"%s\n", path, w->title, copy ? " (copy)" : "");
    wm_focus(w);
    send(w, &e);
}

static void on_motion(void)
{
    switch (drag_mode) {
    case DRAG_DND:
        wm_damage(last_dnd);
        last_dnd = dnd_rect();
        wm_damage(last_dnd);
        return;
    case DRAG_MOVE: {
        int nx = mx - drag_dx, ny = my - drag_dy;
        if (ny < 0) ny = 0;
        if (ny > SCREEN_H - TASKBAR_H - TITLE_H) ny = SCREEN_H - TASKBAR_H - TITLE_H;
        if (nx < 60 - drag_win->frame.w) nx = 60 - drag_win->frame.w;
        if (nx > SCREEN_W - 60) nx = SCREEN_W - 60;
        wm_move(drag_win, nx, ny);
        return;
    }
    case DRAG_RESIZE: {
        rect_t f = drag_frame;
        int dx = mx - press_x, dy = my - press_y;
        int minfw = drag_win->min_w + 2 * BORDER, minfh = drag_win->min_h + 2 * BORDER + TITLE_H;
        if (drag_edges & EDGE_R) f.w += dx;
        if (drag_edges & EDGE_B) f.h += dy;
        if (drag_edges & EDGE_L) { int nw = MAX(minfw, f.w - dx); f.x += f.w - nw; f.w = nw; }
        if (drag_edges & EDGE_T) { int nh = MAX(minfh, f.h - dy); f.y += f.h - nh; f.h = nh; }
        if (f.y < 0) { f.h += f.y; f.y = 0; }
        wm_set_frame(drag_win, f);
        return;
    }
    case DRAG_CAPTURE:
        if (wm_window_alive(drag_win)) mouse_event_to(drag_win, EV_MOUSE_MOVE, 0);
        return;
    case DRAG_TITLE_BTN: {
        int edges, part = HT_NONE;
        window_t *w = window_at(mx, my, &part, &edges);
        set_hover_btn(w == drag_win ? w : NULL, part);
        return;
    }
    case DRAG_DESKTOP:
        desktop_mouse_move(mx, my, mbuttons);
        return;
    }
    /* no drag: hover feedback and cursor shape */
    if (menu_mouse_move(mx, my)) { cur_cursor = CURSOR_ARROW; set_hover_btn(NULL, 0); return; }
    taskbar_mouse_move(mx, my);
    if (rect_contains(taskbar_rect(), mx, my)) { cur_cursor = CURSOR_ARROW; set_hover_btn(NULL, 0); return; }
    int edges = 0, part = HT_NONE;
    window_t *w = window_at(mx, my, &part, &edges);
    set_hover_btn(w, part);
    if (w && part == HT_RESIZE) cur_cursor = edges_cursor(edges);
    else if (w && part == HT_CLIENT) {
        app_cursor = CURSOR_ARROW;
        mouse_event_to(w, EV_MOUSE_MOVE, 0);
        cur_cursor = app_cursor;
    } else cur_cursor = CURSOR_ARROW;
    if (!w) desktop_mouse_move(mx, my, mbuttons);
}

static void on_press(int button)
{
    u64 now = timer_ticks();
    bool dbl = button == last_click_btn && now - last_click_t < (u64)g_dblclick_ms &&
               ABS(mx - last_click_x) <= 4 && ABS(my - last_click_y) <= 4;
    last_click_t = dbl ? 0 : now;
    last_click_x = mx; last_click_y = my; last_click_btn = button;
    press_x = mx; press_y = my;

    if (drag_mode == DRAG_CAPTURE && wm_window_alive(drag_win)) {   /* second button while captured */
        mouse_event_to(drag_win, EV_MOUSE_DOWN, button);
        return;
    }
    if (menu_is_open()) {
        bool on_start = menu_is_banner() && rect_contains(taskbar_start_rect(), mx, my);
        if (menu_mouse_down(mx, my) && !on_start) return;
        if (on_start) return;              /* start button toggles: menu now closed */
    }
    if (rect_contains(taskbar_rect(), mx, my)) {
        drag_mode = DRAG_TASKBAR;
        taskbar_mouse_down(mx, my, button);
        return;
    }
    int edges = 0, part = HT_NONE;
    window_t *w = window_at(mx, my, &part, &edges);
    if (w) {
        if (w->modal && wm_window_alive(w->modal)) { wm_focus(w->modal); return; }
        wm_focus(w);
        if (button != MOUSE_LEFT && part != HT_CLIENT) return;
        switch (part) {
        case HT_RESIZE:
            drag_mode = DRAG_RESIZE; drag_win = w; drag_edges = edges; drag_frame = w->frame;
            cur_cursor = edges_cursor(edges);
            break;
        case HT_TITLE:
            if (dbl && (w->flags & WF_RESIZABLE)) { wm_toggle_maximize(w); break; }
            if (!w->maximized) { drag_mode = DRAG_MOVE; drag_win = w; drag_dx = mx - w->frame.x; drag_dy = my - w->frame.y; }
            break;
        case HT_BTN_CLOSE: case HT_BTN_MAX: case HT_BTN_MIN:
            drag_mode = DRAG_TITLE_BTN; drag_win = w; drag_btn = part;
            wm_damage(window_bounds(w));
            break;
        case HT_CLIENT:
            drag_mode = DRAG_CAPTURE; drag_win = w;
            mouse_event_to(w, EV_MOUSE_DOWN, button);
            if (dbl && wm_window_alive(w)) mouse_event_to(w, EV_DBLCLICK, button);
            break;
        }
        return;
    }
    drag_mode = DRAG_DESKTOP;
    desktop_mouse_down(mx, my, button, dbl);
}

static void on_release(int button)
{
    if (menu_is_open() && drag_mode != DRAG_CAPTURE && menu_mouse_up(mx, my)) {
        if (!mbuttons) drag_mode = DRAG_NONE;
        return;
    }
    switch (drag_mode) {
    case DRAG_DND: {
        if (button != MOUSE_LEFT) break;
        wm_damage(last_dnd);
        drag_mode = DRAG_NONE;
        bool copy = keyboard_mods() & MOD_CTRL;
        window_t *w = wm_window_under(mx, my);
        if (w) wm_send_drop(w, dnd_path, mx, my, copy);
        else if (!rect_contains(taskbar_rect(), mx, my)) desktop_drop(dnd_path, mx, my, copy);
        break;
    }
    case DRAG_CAPTURE:
        if (wm_window_alive(drag_win)) mouse_event_to(drag_win, EV_MOUSE_UP, button);
        if (!mbuttons) drag_mode = DRAG_NONE;
        break;
    case DRAG_TITLE_BTN: {
        int edges, part = HT_NONE;
        window_t *w = window_at(mx, my, &part, &edges);
        window_t *target = drag_win;
        int btn = drag_btn;
        drag_mode = DRAG_NONE;
        if (wm_window_alive(target)) wm_damage(window_bounds(target));
        if (w == target && part == btn) {
            if (btn == HT_BTN_CLOSE) wm_close(target);
            else if (btn == HT_BTN_MAX) wm_toggle_maximize(target);
            else if (btn == HT_BTN_MIN) wm_minimize(target);
        }
        break;
    }
    case DRAG_DESKTOP:
        desktop_mouse_up(mx, my, button);
        if (!mbuttons) drag_mode = DRAG_NONE;
        break;
    case DRAG_MOVE:
    case DRAG_RESIZE:
        if (wm_window_alive(drag_win))
            klog("[wm  ] window %d %s to %d,%d %dx%d\n", drag_win->id, drag_mode == DRAG_MOVE ? "moved" : "resized",
                 drag_win->frame.x, drag_win->frame.y, drag_win->frame.w, drag_win->frame.h);
        if (!mbuttons) drag_mode = DRAG_NONE;
        break;
    default:
        if (!mbuttons) drag_mode = DRAG_NONE;
        break;
    }
    if (!mbuttons) { drag_win = drag_mode == DRAG_NONE ? NULL : drag_win; on_motion(); }
}

static void handle_mouse(struct mouse_event *me)
{
    u8 old = mbuttons;
    bool moved = me->x != mx || me->y != my;
    mx = me->x; my = me->y;
    mbuttons = me->buttons;
    if (moved) on_motion();
    if (me->wheel && !menu_is_open()) {
        int edges, part = HT_NONE;
        window_t *w = drag_mode == DRAG_CAPTURE && wm_window_alive(drag_win) ? drag_win : window_at(mx, my, &part, &edges);
        if (w && !(w->modal && wm_window_alive(w->modal))) {
            event_t e;
            memset(&e, 0, sizeof e);
            rect_t c = wm_client_rect(w);
            e.type = EV_WHEEL;
            e.x = mx - c.x; e.y = my - c.y;
            e.wheel = me->wheel;
            e.buttons = mbuttons;
            e.key.mods = keyboard_mods();
            klog("[wm  ] wheel %d -> \"%s\"\n", me->wheel, w->title);
            send(w, &e);
        }
    }
    u8 pressed = me->buttons & ~old, released = old & ~me->buttons;
    static const int order[3] = { MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE };
    for (int i = 0; i < 3; i++) {
        if (pressed & order[i]) on_press(order[i]);
        if (released & order[i]) on_release(order[i]);
    }
}

/* ---------------------------------------------------------------------
 * keyboard
 * ------------------------------------------------------------------- */
static void alt_tab(void)
{
    /* bring the bottom-most window (including minimized ones) to the front;
     * repeating this cycles through all windows */
    for (int i = 0; i < nwin; i++) {
        window_t *w = zorder[i];
        if (w->destroyed || (w->flags & WF_NO_TASKBAR) || w == focused) continue;
        klog("[wm  ] alt-tab -> \"%s\"\n", w->title);
        wm_focus(w);
        return;
    }
}

static void handle_key(struct key_event *ke)
{
    if (ke->pressed && (ke->key == KEY_LWIN || ke->key == KEY_RWIN)) { taskbar_toggle_start(); return; }
    if (ke->pressed && (ke->mods & MOD_CTRL) && ke->key == KEY_ESC) { taskbar_toggle_start(); return; }
    if (menu_is_open()) { menu_key(ke); return; }
    if (drag_mode == DRAG_DND && ke->pressed && ke->key == KEY_ESC) {
        wm_damage(last_dnd);
        drag_mode = DRAG_NONE;
        klog("[wm  ] drag cancelled\n");
        return;
    }
    if (ke->pressed && (ke->mods & MOD_ALT) && ke->key == KEY_TAB) { alt_tab(); return; }
    if (ke->pressed && (ke->mods & MOD_ALT) && ke->key == KEY_F4) {
        if (focused) wm_close(focused);
        else taskbar_shutdown_prompt();
        return;
    }
    if (focused) {
        event_t e;
        memset(&e, 0, sizeof e);
        e.type = EV_KEY;
        e.key = *ke;
        send(focused, &e);
    } else {
        desktop_key(ke);
    }
}

/* ---------------------------------------------------------------------
 * main loop
 * ------------------------------------------------------------------- */
static void run_timers(void)
{
    u64 now = timer_ticks();
    for (int i = 0; i < nwin; i++) {
        window_t *w = zorder[i];
        if (w->destroyed || !w->timer_ms || now < w->timer_next) continue;
        w->timer_next = now + w->timer_ms;
        send_simple(w, EV_TIMER);
    }
    taskbar_tick();
    desktop_tick();
}

void wm_repaint_all(void)
{
    for (int i = 0; i < nwin; i++) if (!zorder[i]->destroyed) send_simple(zorder[i], EV_PAINT);
    wm_damage(rect_make(0, 0, SCREEN_W, SCREEN_H));
}

/* Size the logical screen for the real screen and the UI scale. */
static bool make_back(int pct)
{
    int lw = (phys_w * 100 + pct - 1) / pct, lh = (phys_h * 100 + pct - 1) / pct;
    if (lw < 320) lw = 320;
    if (lh < 180) lh = 180;
    surface_t *nb = surface_create(lw, lh);
    if (!nb) return false;
    surface_destroy(back);
    back = nb;
    SCREEN_W = lw;
    SCREEN_H = lh;
    ui_pct = pct;
    kfree(map_x);
    kfree(map_y);
    map_x = kmalloc(sizeof(int) * phys_w);
    map_y = kmalloc(sizeof(int) * phys_h);
    for (int x = 0; x < phys_w; x++) map_x[x] = MIN((int)((u64)x * lw / phys_w), lw - 1);
    for (int y = 0; y < phys_h; y++) map_y[y] = MIN((int)((u64)y * lh / phys_h), lh - 1);
    return true;
}

/* After the screen or the scale changed: re-fit windows and redraw everything. */
static void screen_changed(void)
{
    surface_wrap(&screen, g_fb.pixels, g_fb.width, g_fb.height, g_fb.pitch);
    cursor_visible = false;                 /* the saved pixels are stale */
    menu_close();
    ndirty = 0;
    mouse_set_bounds(SCREEN_W, SCREEN_H);
    int x, y;
    u8 b;
    mouse_get(&x, &y, &b);
    mx = x; my = y;
    /* keep every window reachable: refit maximized ones, pull others back on screen */
    for (int i = 0; i < nwin; i++) {
        window_t *win = zorder[i];
        if (win->destroyed) continue;
        if (win->maximized) { wm_set_frame(win, rect_make(0, 0, SCREEN_W, SCREEN_H - TASKBAR_H)); continue; }
        rect_t f = win->frame;
        if (f.w > SCREEN_W) f.w = SCREEN_W;
        if (f.h > SCREEN_H - TASKBAR_H) f.h = SCREEN_H - TASKBAR_H;
        if (f.x + f.w > SCREEN_W) f.x = SCREEN_W - f.w;
        if (f.y + f.h > SCREEN_H - TASKBAR_H) f.y = SCREEN_H - TASKBAR_H - f.h;
        if (f.x < 0) f.x = 0;
        if (f.y < 0) f.y = 0;
        wm_set_frame(win, f);
        if (!win->maximized) win->restore = win->frame;
    }
    taskbar_init();
    desktop_screen_changed();
    ndirty = 0;
    wm_damage(rect_make(0, 0, SCREEN_W, SCREEN_H));
}

bool wm_set_resolution(int w, int h)
{
    if (w == phys_w && h == phys_h) return true;
    int ow = phys_w, oh = phys_h, opct = ui_pct;
    if (!fb_set_mode(w, h)) return false;
    phys_w = w;
    phys_h = h;
    if (!make_back(ui_pct)) { phys_w = ow; phys_h = oh; fb_set_mode(ow, oh); return false; }
    screen_changed();
    klog("[wm  ] resolution changed to %dx%d (UI scale %d%%)\n", w, h, opct);
    return true;
}

void wm_set_ui_scale(int pct)
{
    if (pct < 50) pct = 50;
    if (pct > 200) pct = 200;
    if (pct == ui_pct) return;
    if (!make_back(pct)) return;
    screen_changed();
    klog("[wm  ] UI scale %d%%\n", pct);
}

int wm_ui_scale(void) { return ui_pct; }

/* Use an image file (BMP, up to 32x32, magenta is transparent) as the pointer. */
bool wm_set_cursor_file(const char *path)
{
    surface_t *img = bmp_load(path);
    if (!img) return false;
    if (img->w > CUR_IMG_MAX || img->h > CUR_IMG_MAX) { surface_destroy(img); return false; }
    cursor_hide();
    surface_destroy(cur_img);
    cur_img = img;
    cursor_drawn_shape = -1;
    cursor_x = -100;
    klog("[wm  ] pointer image %s (%dx%d)\n", path, img->w, img->h);
    return true;
}

void wm_set_cursor_look(int style, int scale, u32 fill)
{
    cursor_hide();
    ptr_style = style % 5;
    cur_scale = scale < 1 ? 1 : scale > 3 ? 3 : scale;
    cur_fill = fill;
    cursor_drawn_shape = -1;                /* make the next frame redraw the pointer */
    cursor_x = -100;
}

void wm_init(void)
{
    surface_wrap(&screen, g_fb.pixels, g_fb.width, g_fb.height, g_fb.pitch);
    phys_w = g_fb.width;
    phys_h = g_fb.height;
    int pct = settings_get_int("ui_scale", 100);
    if (!make_back(pct < 50 || pct > 200 ? 100 : pct)) panic("wm: cannot allocate back buffer");
    mouse_set_bounds(SCREEN_W, SCREEN_H);
    int x, y;
    u8 b;
    mouse_get(&x, &y, &b);
    mx = x; my = y;
    mouse_set_speed(settings_get_int("mouse_speed", 4));
    const char *cfile = settings_get("cursor_file", NULL);
    if (cfile && settings_get_int("cursor_style", 0) == 4) wm_set_cursor_file(cfile);
    wm_set_cursor_look(settings_get_int("cursor_style", 0), settings_get_int("cursor_scale", 1),
                       (u32)settings_get_int("cursor_fill", 0xFFFFFF));
    g_ui_smooth = settings_get_int("smooth_fonts", 1);
    g_dblclick_ms = settings_get_int("dblclick_ms", 450);
    icons_init();
    desktop_init();
    taskbar_init();
    wm_damage(rect_make(0, 0, SCREEN_W, SCREEN_H));
    klog("[wm  ] window manager ready\n");
}

void wm_run(void)
{
    for (;;) {
        bool activity = false;
        struct mouse_event me;
        while (mouse_poll(&me)) { handle_mouse(&me); activity = true; }
        struct key_event ke;
        while (keyboard_poll(&ke)) { handle_key(&ke); activity = true; }
        run_timers();
        reap();
        render();
        if (!activity) task_sleep(8);
    }
}
