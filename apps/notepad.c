/*
 * NimbusOS - Notepad: a multi-line plain text editor.
 *
 * The document is one gap-less byte buffer plus a table of line start
 * offsets that is rebuilt after every edit (fine for files up to a few
 * hundred KiB).  Supports cursor movement, shift/mouse selection, insert,
 * delete, cut/copy/paste (system clipboard), vertical + horizontal
 * scrolling, and New / Open / Save / Save As with unsaved-change prompts.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/dialog.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "gui/clipboard.h"
#include "fs/vfs.h"
#include "drivers/pit.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define TOOLBAR_H 32
#define STATUS_H 22
#define SB_W 16
#define MARGIN 4
#define LINE_H 16

enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, NBTN };
enum { AFTER_NONE, AFTER_NEW, AFTER_OPEN, AFTER_CLOSE };

struct notepad {
    char *buf;
    u32 len, cap;
    u32 *lines;             /* start offset of each line */
    u32 nlines, lines_cap;
    u32 cursor, anchor;     /* selection = [min(anchor,cursor), max) */
    int top, left;          /* first visible line / column */
    int want_col;           /* column to keep when moving vertically */
    char path[NBFS_PATH_MAX];
    bool dirty, blink, mouse_sel, drag_sb;
    int after_save;         /* what to do once a save prompt completes */
    ui_btn_t btn[NBTN];
    int hover, pressed;
};

/* ---------------------------------------------------------------- text */
static void reindex(struct notepad *n)
{
    u32 count = 1;
    for (u32 i = 0; i < n->len; i++) if (n->buf[i] == '\n') count++;
    if (count > n->lines_cap) {
        n->lines_cap = count + 64;
        kfree(n->lines);
        n->lines = kmalloc(n->lines_cap * sizeof(u32));
    }
    n->nlines = 0;
    n->lines[n->nlines++] = 0;
    for (u32 i = 0; i < n->len; i++) if (n->buf[i] == '\n') n->lines[n->nlines++] = i + 1;
}

static u32 line_of(struct notepad *n, u32 pos)
{
    u32 lo = 0, hi = n->nlines - 1;
    while (lo < hi) {
        u32 mid = (lo + hi + 1) / 2;
        if (n->lines[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static u32 line_end(struct notepad *n, u32 line)
{
    return line + 1 < n->nlines ? n->lines[line + 1] - 1 : n->len;
}

static bool reserve(struct notepad *n, u32 extra)
{
    if (n->len + extra + 1 <= n->cap) return true;
    u32 nc = MAX(n->cap * 2, n->len + extra + 1024);
    char *nb = krealloc(n->buf, nc);
    if (!nb) return false;
    n->buf = nb;
    n->cap = nc;
    return true;
}

static bool has_sel(struct notepad *n) { return n->anchor != n->cursor; }
static u32 sel_a(struct notepad *n) { return MIN(n->anchor, n->cursor); }
static u32 sel_b(struct notepad *n) { return MAX(n->anchor, n->cursor); }

static void delete_range(struct notepad *n, u32 a, u32 b)
{
    if (b <= a) return;
    memmove(n->buf + a, n->buf + b, n->len - b);
    n->len -= b - a;
    n->cursor = n->anchor = a;
    n->dirty = true;
}

static void insert(struct notepad *n, const char *s, u32 len)
{
    if (has_sel(n)) delete_range(n, sel_a(n), sel_b(n));
    if (!reserve(n, len)) return;
    memmove(n->buf + n->cursor + len, n->buf + n->cursor, n->len - n->cursor);
    memcpy(n->buf + n->cursor, s, len);
    n->len += len;
    n->cursor += len;
    n->anchor = n->cursor;
    n->dirty = true;
}

/* ---------------------------------------------------------------- view */
static rect_t text_rect(window_t *w)
{
    return rect_make(0, TOOLBAR_H, w->surface->w - SB_W, w->surface->h - TOOLBAR_H - STATUS_H);
}
static int vis_lines(window_t *w) { return MAX(1, (text_rect(w).h - 2 * MARGIN) / LINE_H); }
static int vis_cols(window_t *w) { return MAX(1, (text_rect(w).w - 2 * MARGIN) / FONT_W); }

static void scroll_to_cursor(window_t *w)
{
    struct notepad *n = w->app;
    int line = line_of(n, n->cursor), col = n->cursor - n->lines[line];
    int vl = vis_lines(w), vc = vis_cols(w);
    if (line < n->top) n->top = line;
    if (line >= n->top + vl) n->top = line - vl + 1;
    if (col < n->left) n->left = MAX(0, col - 8);
    if (col >= n->left + vc) n->left = col - vc + 8;
}

static void update_title(window_t *w)
{
    struct notepad *n = w->app;
    char t[96];
    snprintf(t, sizeof t, "%s%s - Notepad", n->dirty ? "*" : "", n->path[0] ? nbfs_path_basename(n->path) : "Untitled");
    if (strcmp(t, w->title)) wm_set_title(w, t);
}

static void draw_toolbar_glyph(surface_t *s, rect_t r, int which)
{
    int x = r.x + 6, y = r.y + (r.h - 16) / 2;
    if (which == B_NEW) icon_draw_small(s, ICON_FILE, x, y);
    else if (which == B_OPEN) icon_draw_small(s, ICON_FOLDER, x, y);
    else icon_draw_small(s, ICON_DRIVE, x, y);
}

static void paint(window_t *w)
{
    struct notepad *n = w->app;
    surface_t *s = w->surface;
    ui_toolbar(s, rect_make(0, 0, s->w, TOOLBAR_H));
    for (int i = 0; i < NBTN; i++) {
        int st = n->pressed == i ? BTN_PRESSED : n->hover == i ? BTN_HOVER : BTN_NORMAL;
        gfx_fill_rect(s, n->btn[i].r.x, n->btn[i].r.y, n->btn[i].r.w, n->btn[i].r.h, RGB(240, 241, 245));
        ui_icon_button(s, n->btn[i].r, -1, NULL, st);
        draw_toolbar_glyph(s, n->btn[i].r, i);
        gfx_text(s, n->btn[i].r.x + 26, n->btn[i].r.y + (n->btn[i].r.h - 16) / 2, n->btn[i].label, C_TEXT, COLOR_NONE);
    }

    rect_t tr = text_rect(w);
    gfx_fill_rect(s, tr.x, tr.y, tr.w, tr.h, 0xFFFFFF);
    rect_t old = s->clip;
    gfx_set_clip(s, tr);
    int vl = vis_lines(w) + 1, vc = vis_cols(w) + 1;
    u32 a = sel_a(n), b = sel_b(n);
    for (int i = 0; i < vl && n->top + i < (int)n->nlines; i++) {
        u32 line = n->top + i;
        u32 start = n->lines[line], end = line_end(n, line);
        int y = tr.y + MARGIN + i * LINE_H;
        for (int c = 0; c < vc; c++) {
            u32 pos = start + n->left + c;
            int x = tr.x + MARGIN + c * FONT_W;
            bool sel = pos >= a && pos < b && pos <= end;
            if (pos > end) break;
            if (sel) gfx_fill_rect(s, x, y, FONT_W, LINE_H, C_SELECT);
            if (pos < end) {
                char ch = n->buf[pos];
                if (ch == '\t') ch = ' ';
                gfx_char(s, x, y, (u8)ch, sel ? 0xFFFFFF : C_TEXT, COLOR_NONE);
            }
        }
    }
    /* caret */
    u32 cl = line_of(n, n->cursor);
    int cx = tr.x + MARGIN + ((int)(n->cursor - n->lines[cl]) - n->left) * FONT_W;
    int cy = tr.y + MARGIN + ((int)cl - n->top) * LINE_H;
    if (n->blink && w == wm_focused()) gfx_fill_rect(s, cx, cy, 2, LINE_H, C_TEXT);
    s->clip = old;
    ui_vscroll(s, rect_make(s->w - SB_W, TOOLBAR_H, SB_W, tr.h), n->nlines, vis_lines(w), n->top);

    int sy = s->h - STATUS_H;
    gfx_fill_rect(s, 0, sy, s->w, STATUS_H, RGB(240, 240, 240));
    gfx_hline(s, 0, sy, s->w, RGB(215, 215, 220));
    char st[128];
    snprintf(st, sizeof st, "Ln %u, Col %u   |   %u lines, %u bytes%s", cl + 1, n->cursor - n->lines[cl] + 1,
             n->nlines, n->len, n->dirty ? "   |   modified" : "");
    gfx_text(s, 8, sy + 3, st, C_TEXT, COLOR_NONE);
    w->mem_hint = n->cap + n->lines_cap * 4;
    update_title(w);
    wm_invalidate(w);
}

/* ---------------------------------------------------------------- files */
static void set_text(window_t *w, const char *text, u32 len)
{
    struct notepad *n = w->app;
    n->len = 0;
    reserve(n, len);
    for (u32 i = 0; i < len; i++) if (text[i] != '\r') n->buf[n->len++] = text[i];
    n->cursor = n->anchor = 0;
    n->top = n->left = 0;
    n->dirty = false;
    reindex(n);
}

static bool load_file(window_t *w, const char *path)
{
    char *data;
    u32 len;
    int r = vfs_read_all(path, &data, &len);
    if (r) {
        char m[200];
        snprintf(m, sizeof m, "Cannot open \"%s\": %s.", path, nbfs_strerror(r));
        msgbox(w, "Notepad", m, ICON_ERROR, MB_OK, NULL, NULL);
        return false;
    }
    set_text(w, data, len);
    kfree(data);
    strlcpy(((struct notepad *)w->app)->path, path, NBFS_PATH_MAX);
    klog("[note] opened %s (%u bytes)\n", path, len);
    return true;
}

static void continue_after_save(window_t *w);

static bool save_to(window_t *w, const char *path)
{
    struct notepad *n = w->app;
    int r = vfs_write_all(path, n->buf, n->len);
    if (r) {
        char m[200];
        snprintf(m, sizeof m, "Cannot save \"%s\": %s.", path, nbfs_strerror(r));
        msgbox(w, "Notepad", m, ICON_ERROR, MB_OK, NULL, NULL);
        return false;
    }
    strlcpy(n->path, path, sizeof n->path);
    n->dirty = false;
    klog("[note] saved %s (%u bytes)\n", path, n->len);
    paint(w);
    return true;
}

static void open_chosen(bool ok, const char *path, void *ctx)
{
    window_t *w = ctx;
    if (!ok || !wm_window_alive(w)) return;
    if (load_file(w, path)) paint(w);
}

static void save_as_chosen(bool ok, const char *path, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    struct notepad *n = w->app;
    if (!ok) { n->after_save = AFTER_NONE; return; }
    if (save_to(w, path)) continue_after_save(w);
}

static void save_as(window_t *w)
{
    struct notepad *n = w->app;
    char dir[NBFS_PATH_MAX];
    if (n->path[0]) nbfs_path_parent(n->path, dir, sizeof dir);
    else strcpy(dir, "/Documents");
    filedialog(w, FD_SAVE, "Save As", dir, n->path[0] ? nbfs_path_basename(n->path) : "Untitled.txt", ".txt",
               save_as_chosen, w);
}

static void save(window_t *w)
{
    struct notepad *n = w->app;
    if (n->path[0]) { if (save_to(w, n->path)) continue_after_save(w); }
    else save_as(w);
}

static void do_open_dialog(window_t *w)
{
    struct notepad *n = w->app;
    char dir[NBFS_PATH_MAX];
    if (n->path[0]) nbfs_path_parent(n->path, dir, sizeof dir);
    else strcpy(dir, "/Documents");
    filedialog(w, FD_OPEN, "Open", dir, "", NULL, open_chosen, w);
}

static void continue_after_save(window_t *w)
{
    struct notepad *n = w->app;
    int what = n->after_save;
    n->after_save = AFTER_NONE;
    if (what == AFTER_NEW) { set_text(w, "", 0); n->path[0] = 0; paint(w); }
    else if (what == AFTER_OPEN) do_open_dialog(w);
    else if (what == AFTER_CLOSE) wm_destroy_window(w);
}

static void prompt_cb(int r, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    struct notepad *n = w->app;
    if (r == DLG_YES) save(w);
    else if (r == DLG_NO) { n->dirty = false; continue_after_save(w); }
    else n->after_save = AFTER_NONE;
}

/* Runs `what` now, or asks to save first when there are unsaved changes. */
static void guarded(window_t *w, int what)
{
    struct notepad *n = w->app;
    n->after_save = what;
    if (!n->dirty) { continue_after_save(w); return; }
    char q[200];
    snprintf(q, sizeof q, "Do you want to save changes to %s?", n->path[0] ? nbfs_path_basename(n->path) : "Untitled");
    msgbox(w, "Notepad", q, ICON_WARNING, MB_YESNOCANCEL, prompt_cb, w);
}

/* ---------------------------------------------------------------- input */
static u32 pos_from_point(window_t *w, int x, int y)
{
    struct notepad *n = w->app;
    rect_t tr = text_rect(w);
    int line = n->top + (y - tr.y - MARGIN) / LINE_H;
    if (y < tr.y + MARGIN) line = n->top - 1;
    line = MAX(0, MIN((int)n->nlines - 1, line));
    int col = n->left + (x - tr.x - MARGIN + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    u32 start = n->lines[line], end = line_end(n, line);
    return MIN(start + col, end);
}

static void move_cursor(window_t *w, u32 pos, bool extend, bool keep_col)
{
    struct notepad *n = w->app;
    n->cursor = pos;
    if (!extend) n->anchor = pos;
    if (!keep_col) n->want_col = pos - n->lines[line_of(n, pos)];
}

static void copy_sel(struct notepad *n)
{
    if (has_sel(n)) clipboard_set(n->buf + sel_a(n), sel_b(n) - sel_a(n));
}

static void handle_key(window_t *w, struct key_event *k)
{
    struct notepad *n = w->app;
    bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
    u32 line = line_of(n, n->cursor);
    bool edited = false;
    if (ctrl) {
        switch (k->key) {
        case 'a': n->anchor = 0; n->cursor = n->len; break;
        case 'c': copy_sel(n); break;
        case 'x': copy_sel(n); if (has_sel(n)) { delete_range(n, sel_a(n), sel_b(n)); edited = true; } break;
        case 'v': {
            u32 len;
            const char *c = clipboard_get(&len);
            if (c && len) { insert(n, c, len); edited = true; }
            break;
        }
        case 's': if (shift) save_as(w); else save(w); return;
        case 'o': guarded(w, AFTER_OPEN); return;
        case 'n': guarded(w, AFTER_NEW); return;
        case KEY_HOME: move_cursor(w, 0, shift, false); break;
        case KEY_END: move_cursor(w, n->len, shift, false); break;
        default: return;
        }
    } else {
        switch (k->key) {
        case KEY_LEFT:
            if (has_sel(n) && !shift) move_cursor(w, sel_a(n), false, false);
            else if (n->cursor > 0) move_cursor(w, n->cursor - 1, shift, false);
            break;
        case KEY_RIGHT:
            if (has_sel(n) && !shift) move_cursor(w, sel_b(n), false, false);
            else if (n->cursor < n->len) move_cursor(w, n->cursor + 1, shift, false);
            break;
        case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
            int d = k->key == KEY_UP ? -1 : k->key == KEY_DOWN ? 1 : k->key == KEY_PGUP ? -vis_lines(w) : vis_lines(w);
            int nl = MAX(0, MIN((int)n->nlines - 1, (int)line + d));
            u32 pos = MIN(n->lines[nl] + n->want_col, line_end(n, nl));
            move_cursor(w, pos, shift, true);
            if (k->key == KEY_PGUP || k->key == KEY_PGDN) n->top = MAX(0, MIN((int)n->nlines - 1, n->top + d));
            break;
        }
        case KEY_HOME: move_cursor(w, n->lines[line], shift, false); break;
        case KEY_END: move_cursor(w, line_end(n, line), shift, false); break;
        case KEY_BACKSPACE:
            if (has_sel(n)) delete_range(n, sel_a(n), sel_b(n));
            else if (n->cursor > 0) delete_range(n, n->cursor - 1, n->cursor);
            edited = true;
            break;
        case KEY_DELETE:
            if (has_sel(n)) delete_range(n, sel_a(n), sel_b(n));
            else if (n->cursor < n->len) delete_range(n, n->cursor, n->cursor + 1);
            edited = true;
            break;
        case KEY_ENTER: insert(n, "\n", 1); edited = true; break;
        case KEY_TAB: insert(n, "    ", 4); edited = true; break;
        default:
            if (k->ch >= 32 && k->ch < 127) { insert(n, &k->ch, 1); edited = true; }
            else return;
        }
    }
    if (edited) {
        reindex(n);
        n->want_col = n->cursor - n->lines[line_of(n, n->cursor)];
    }
    n->blink = true;
    scroll_to_cursor(w);
    paint(w);
}

static void layout(window_t *w)
{
    struct notepad *n = w->app;
    static const char *labels[NBTN] = { "New", "Open", "Save", "Save As" };
    int x = 4;
    for (int i = 0; i < NBTN; i++) {
        int bw = 34 + gfx_text_width(labels[i]);
        n->btn[i] = (ui_btn_t){ rect_make(x, 3, bw, TOOLBAR_H - 6), i, labels[i], -1, false };
        x += bw + 4;
    }
}

static void on_event(window_t *w, event_t *ev)
{
    struct notepad *n = w->app;
    rect_t tr = text_rect(w);
    rect_t sb = rect_make(w->surface->w - SB_W, TOOLBAR_H, SB_W, tr.h);
    switch (ev->type) {
    case EV_PAINT: layout(w); scroll_to_cursor(w); paint(w); break;
    case EV_FOCUS: case EV_BLUR: paint(w); break;
    case EV_TIMER: n->blink = !n->blink; paint(w); break;
    case EV_KEY: if (ev->key.pressed) handle_key(w, &ev->key); break;
    case EV_MOUSE_DOWN:
        n->pressed = -1;
        if (ev->button == MOUSE_LEFT && (n->pressed = ui_btn_hit(n->btn, NBTN, ev->x, ev->y)) >= 0) { paint(w); break; }
        if (rect_contains(sb, ev->x, ev->y)) {
            n->drag_sb = true;
            n->top = ui_vscroll_offset_for(sb, n->nlines, vis_lines(w), ev->y);
            paint(w);
        } else if (rect_contains(tr, ev->x, ev->y) && ev->button == MOUSE_LEFT) {
            bool shift = keyboard_mods() & MOD_SHIFT;
            move_cursor(w, pos_from_point(w, ev->x, ev->y), shift, false);
            n->mouse_sel = true;
            n->blink = true;
            paint(w);
        }
        break;
    case EV_DBLCLICK:
        if (rect_contains(tr, ev->x, ev->y)) {   /* select the word under the mouse */
            u32 a = n->cursor, b = n->cursor;
            while (a > 0 && (isalnum(n->buf[a - 1]) || n->buf[a - 1] == '_')) a--;
            while (b < n->len && (isalnum(n->buf[b]) || n->buf[b] == '_')) b++;
            n->anchor = a; n->cursor = b;
            n->mouse_sel = false;
            paint(w);
        }
        break;
    case EV_MOUSE_MOVE:
        if (n->drag_sb) {
            n->top = ui_vscroll_offset_for(sb, n->nlines, vis_lines(w), ev->y);
            paint(w);
        } else if (n->mouse_sel && (ev->buttons & MOUSE_LEFT)) {
            n->cursor = pos_from_point(w, ev->x, ev->y);
            scroll_to_cursor(w);
            paint(w);
        } else {
            int h = ui_btn_hit(n->btn, NBTN, ev->x, ev->y);
            if (h != n->hover) { n->hover = h; paint(w); }
            if (rect_contains(tr, ev->x, ev->y)) wm_set_app_cursor(CURSOR_TEXT);
        }
        break;
    case EV_MOUSE_UP: {
        n->mouse_sel = n->drag_sb = false;
        int p = n->pressed;
        n->pressed = -1;
        if (p >= 0 && ui_btn_hit(n->btn, NBTN, ev->x, ev->y) == p) {
            if (p == B_NEW) guarded(w, AFTER_NEW);
            else if (p == B_OPEN) guarded(w, AFTER_OPEN);
            else if (p == B_SAVE) save(w);
            else save_as(w);
        }
        paint(w);
        break;
    }
    case EV_CLOSE: guarded(w, AFTER_CLOSE); break;
    case EV_DESTROY: kfree(n->buf); kfree(n->lines); kfree(n); break;
    }
}

void notepad_open(const char *path)
{
    struct notepad *n = kzalloc(sizeof *n);
    if (!n) return;
    n->hover = n->pressed = -1;
    reserve(n, 1024);
    reindex(n);
    window_t *w = wm_create("Untitled - Notepad", ICON_NOTEPAD, 620, 420, WF_RESIZABLE, on_event, n);
    if (!w) { kfree(n->buf); kfree(n->lines); kfree(n); return; }
    w->min_w = 320;
    w->min_h = 160;
    if (path && path[0]) load_file(w, path);
    n->blink = true;
    layout(w);
    paint(w);
    wm_set_timer(w, 530);
}
