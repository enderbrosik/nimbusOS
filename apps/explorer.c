/*
 * NimbusOS - File Explorer.
 *
 * Toolbar (Back / Up / Refresh + address bar), a "places" sidebar, and a
 * details list of the current folder with icons.  Double-click opens a
 * folder or opens a file with its associated app.  Right-click shows
 * Open / Rename / Delete / Properties for an item and New Folder /
 * New Text File / Refresh / Open Terminal Here for the folder background.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/dialog.h"
#include "gui/menu.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/pit.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define TOOLBAR_H 36
#define STATUS_H 22
#define SIDEBAR_W 132
#define HEADER_H 22
#define ROW_H 22
#define HISTORY 32

enum { B_BACK, B_UP, B_REFRESH, NBTN };
enum { M_OPEN = 1, M_RENAME, M_DELETE, M_PROPS, M_NEW_FOLDER, M_NEW_TEXT, M_REFRESH, M_TERMINAL };

static const struct { const char *label; const char *path; int icon; } places[] = {
    { "System Disk", "/", ICON_DRIVE },
    { "Desktop", "/Desktop", ICON_FOLDER },
    { "Documents", "/Documents", ICON_FOLDER },
    { "Pictures", "/Pictures", ICON_FOLDER },
    { "System", "/System", ICON_FOLDER },
};

struct explorer {
    char path[NBFS_PATH_MAX];
    char history[HISTORY][NBFS_PATH_MAX];
    int nhist;
    struct vfs_item *items;
    int n, sel, scroll;
    ui_btn_t btn[NBTN];
    int hover, pressed;
    bool drag_scroll;
    int drag_cand, press_x, press_y;   /* row that may become a drag */
    u32 signature;
    char pending[NBFS_PATH_MAX];   /* target of rename/delete dialogs */
};

static rect_t list_rect(window_t *w)
{
    return rect_make(SIDEBAR_W, TOOLBAR_H + HEADER_H, w->surface->w - SIDEBAR_W - 16,
                     w->surface->h - TOOLBAR_H - HEADER_H - STATUS_H);
}
static rect_t scroll_rect(window_t *w)
{
    rect_t l = list_rect(w);
    return rect_make(l.x + l.w, TOOLBAR_H, 16, l.h + HEADER_H);
}
static int visible_rows(window_t *w) { return MAX(1, list_rect(w).h / ROW_H); }

static u32 signature_of(struct vfs_item *it, int n)
{
    u32 h = 2166136261u ^ (u32)n;
    for (int i = 0; i < n; i++) {
        for (const char *p = it[i].name; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        h = (h ^ it[i].size ^ it[i].mtime) * 16777619u;
    }
    return h;
}

static void load(window_t *w, bool keep_sel)
{
    struct explorer *e = w->app;
    char selname[NBFS_NAME_MAX + 1] = "";
    if (keep_sel && e->sel >= 0 && e->sel < e->n) strlcpy(selname, e->items[e->sel].name, sizeof selname);
    kfree(e->items);
    e->items = NULL;
    int n = vfs_list(e->path, &e->items);
    if (n < 0) {
        /* folder vanished: fall back to the root */
        strcpy(e->path, "/");
        n = vfs_list(e->path, &e->items);
    }
    e->n = MAX(0, n);
    e->signature = signature_of(e->items, e->n);
    e->sel = -1;
    if (!keep_sel) e->scroll = 0;
    for (int i = 0; i < e->n && selname[0]; i++) if (!strcmp(e->items[i].name, selname)) e->sel = i;
    int maxs = MAX(0, e->n - visible_rows(w));
    if (e->scroll > maxs) e->scroll = maxs;
    w->mem_hint = e->n * sizeof(struct vfs_item);
    char title[96];
    snprintf(title, sizeof title, "File Explorer - %s", e->path);
    wm_set_title(w, title);
}

static void draw_arrow(surface_t *s, rect_t r, int dir, u32 c)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (dir == 0) {                    /* left arrow */
        for (int i = 0; i < 6; i++) gfx_vline(s, cx - 5 + i, cy - i, 2 * i + 1, c);
        gfx_fill_rect(s, cx + 1, cy - 2, 6, 5, c);
    } else if (dir == 1) {             /* up arrow */
        for (int i = 0; i < 6; i++) gfx_hline(s, cx - i, cy - 5 + i, 2 * i + 1, c);
        gfx_fill_rect(s, cx - 2, cy + 1, 5, 6, c);
    } else {                           /* refresh: circle with a notch */
        gfx_circle(s, cx, cy, 6, c);
        gfx_circle(s, cx, cy, 5, c);
        gfx_fill_rect(s, cx + 1, cy - 7, 7, 6, color_lighten(c, 255));
        for (int i = 0; i < 4; i++) gfx_hline(s, cx + 1 + i, cy - 6 + i, 7 - 2 * i, c);
    }
}

static void paint(window_t *w)
{
    struct explorer *e = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFF);

    /* toolbar */
    ui_toolbar(s, rect_make(0, 0, s->w, TOOLBAR_H));
    for (int i = 0; i < NBTN; i++) {
        ui_btn_t *b = &e->btn[i];
        int st = b->disabled ? BTN_DISABLED : e->pressed == i ? BTN_PRESSED : e->hover == i ? BTN_HOVER : BTN_NORMAL;
        ui_icon_button(s, b->r, ICON_NONE, NULL, st);
        draw_arrow(s, b->r, i, b->disabled ? RGB(180, 180, 180) : RGB(40, 90, 170));
    }
    rect_t addr = rect_make(110, 6, s->w - 120, 24);
    ui_sunken(s, addr, 0xFFFFFF);
    icon_draw_small(s, ICON_FOLDER, addr.x + 4, addr.y + 4);
    gfx_ui_text_ellipsis(s, addr.x + 26, addr.y + 4, e->path, addr.w - 30, C_TEXT);

    /* sidebar */
    gfx_fill_rect(s, 0, TOOLBAR_H, SIDEBAR_W, s->h - TOOLBAR_H - STATUS_H, RGB(244, 247, 252));
    gfx_vline(s, SIDEBAR_W - 1, TOOLBAR_H, s->h - TOOLBAR_H - STATUS_H, RGB(220, 224, 232));
    gfx_ui_text_bold(s, 10, TOOLBAR_H + 8, "Places", C_TEXT_DIM);
    for (unsigned i = 0; i < ARRAY_SIZE(places); i++) {
        int y = TOOLBAR_H + 30 + i * 24;
        if (!strcmp(e->path, places[i].path)) gfx_fill_rect(s, 4, y - 3, SIDEBAR_W - 9, 22, C_SELECT_LIGHT);
        icon_draw_small(s, places[i].icon, 10, y);
        gfx_ui_text(s, 32, y, places[i].label, C_TEXT);
    }

    /* header */
    rect_t lr = list_rect(w);
    int name_w = lr.w - 260;
    gfx_gradient_v(s, lr.x, TOOLBAR_H, lr.w, HEADER_H, 0xFFFFFF, RGB(240, 241, 245));
    gfx_hline(s, lr.x, TOOLBAR_H + HEADER_H - 1, lr.w, RGB(220, 222, 228));
    gfx_ui_text(s, lr.x + 30, TOOLBAR_H + 3, "Name", C_TEXT_DIM);
    gfx_ui_text(s, lr.x + name_w + 10, TOOLBAR_H + 3, "Size", C_TEXT_DIM);
    gfx_ui_text(s, lr.x + name_w + 100, TOOLBAR_H + 3, "Modified", C_TEXT_DIM);
    ui_separator_v(s, lr.x + name_w + 2, TOOLBAR_H + 3, HEADER_H - 6);
    ui_separator_v(s, lr.x + name_w + 92, TOOLBAR_H + 3, HEADER_H - 6);

    /* rows */
    rect_t old = s->clip;
    gfx_set_clip(s, lr);
    int rows = visible_rows(w) + 1;
    for (int i = 0; i < rows && e->scroll + i < e->n; i++) {
        int idx = e->scroll + i;
        struct vfs_item *it = &e->items[idx];
        int y = lr.y + i * ROW_H;
        if (idx == e->sel) {
            gfx_fill_rect(s, lr.x + 2, y + 1, lr.w - 4, ROW_H - 2, C_SELECT_LIGHT);
            gfx_rect(s, lr.x + 2, y + 1, lr.w - 4, ROW_H - 2, RGB(153, 209, 255));
        }
        bool dir = it->type == NBFS_TYPE_DIR;
        icon_draw_small(s, icon_for_file(it->name, dir), lr.x + 8, y + 3);
        gfx_ui_text_ellipsis(s, lr.x + 30, y + 3, it->name, name_w - 34, C_TEXT);
        char buf[32];
        if (!dir) {
            vfs_format_size(it->size, buf, sizeof buf);
            gfx_ui_text(s, lr.x + name_w + 10, y + 3, buf, C_TEXT_DIM);
        } else {
            gfx_ui_text(s, lr.x + name_w + 10, y + 3, "Folder", C_TEXT_DIM);
        }
        vfs_format_time(it->mtime, buf, sizeof buf);
        gfx_ui_text(s, lr.x + name_w + 100, y + 3, buf, C_TEXT_DIM);
    }
    if (e->n == 0) gfx_ui_text(s, lr.x + 30, lr.y + 10, "This folder is empty.", C_TEXT_DIM);
    s->clip = old;
    ui_vscroll(s, scroll_rect(w), e->n, visible_rows(w), e->scroll);

    /* status bar */
    int sy = s->h - STATUS_H;
    gfx_fill_rect(s, 0, sy, s->w, STATUS_H, RGB(240, 240, 240));
    gfx_hline(s, 0, sy, s->w, RGB(215, 215, 220));
    char st[128];
    if (e->sel >= 0) {
        char sz[24];
        vfs_format_size(e->items[e->sel].size, sz, sizeof sz);
        snprintf(st, sizeof st, "%d items   |   \"%s\" selected%s%s", e->n, e->items[e->sel].name,
                 e->items[e->sel].type == NBFS_TYPE_DIR ? "" : ", ", e->items[e->sel].type == NBFS_TYPE_DIR ? "" : sz);
    } else {
        u32 total, freeb;
        if (vfs_is_usb(e->path)) {
            int drv;
            fat32_info(&drv, &total, &freeb);
            snprintf(st, sizeof st, "%d items   |   %u MiB free on USB Disk", e->n, freeb >> 10);
        } else {
            nbfs_statfs(ROOTFS, &total, &freeb);
            snprintf(st, sizeof st, "%d items   |   %u MiB free on System Disk", e->n, freeb >> 20);
        }
    }
    gfx_ui_text_ellipsis(s, 8, sy + 3, st, s->w - 16, C_TEXT);
    wm_invalidate(w);
}

static void layout(window_t *w)
{
    struct explorer *e = w->app;
    e->btn[B_BACK] = (ui_btn_t){ rect_make(6, 5, 30, 26), B_BACK, NULL, -1, e->nhist == 0 };
    e->btn[B_UP] = (ui_btn_t){ rect_make(40, 5, 30, 26), B_UP, NULL, -1, !strcmp(e->path, "/") };
    e->btn[B_REFRESH] = (ui_btn_t){ rect_make(74, 5, 30, 26), B_REFRESH, NULL, -1, false };
}

static void navigate(window_t *w, const char *path, bool push)
{
    struct explorer *e = w->app;
    if (!vfs_is_dir(path)) return;
    if (push && strcmp(path, e->path)) {
        if (e->nhist == HISTORY) { memmove(e->history[0], e->history[1], (HISTORY - 1) * NBFS_PATH_MAX); e->nhist--; }
        strlcpy(e->history[e->nhist++], e->path, NBFS_PATH_MAX);
    }
    strlcpy(e->path, path, sizeof e->path);
    klog("[expl] navigate %s\n", e->path);
    load(w, false);
    layout(w);
    paint(w);
}

static void go_up(window_t *w)
{
    struct explorer *e = w->app;
    char up[NBFS_PATH_MAX];
    nbfs_path_parent(e->path, up, sizeof up);
    navigate(w, up, true);
}

static void go_back(window_t *w)
{
    struct explorer *e = w->app;
    if (!e->nhist) return;
    char p[NBFS_PATH_MAX];
    strlcpy(p, e->history[--e->nhist], sizeof p);
    navigate(w, p, false);
}

static void item_path(struct explorer *e, int i, char *out)
{
    nbfs_path_join(e->path, e->items[i].name, out, NBFS_PATH_MAX);
}

static void open_item(window_t *w, int i)
{
    struct explorer *e = w->app;
    if (i < 0 || i >= e->n) return;
    char p[NBFS_PATH_MAX];
    item_path(e, i, p);
    if (e->items[i].type == NBFS_TYPE_DIR) navigate(w, p, true);
    else app_open_path(p);
}

static void refresh(window_t *w) { load(w, true); layout(w); paint(w); }

/* ---- dialogs ---- */
static void rename_done(bool ok, const char *text, void *ctx)
{
    window_t *w = ctx;
    if (!ok || !wm_window_alive(w) || !text[0]) return;
    struct explorer *e = w->app;
    char np[NBFS_PATH_MAX];
    nbfs_path_join(e->path, text, np, sizeof np);
    int r = vfs_rename(e->pending, np);
    if (r) {
        char m[160];
        snprintf(m, sizeof m, "Cannot rename \"%s\": %s.", nbfs_path_basename(e->pending), nbfs_strerror(r));
        msgbox(w, "Rename", m, ICON_ERROR, MB_OK, NULL, NULL);
    } else {
        klog("[expl] renamed %s -> %s\n", e->pending, np);
    }
    load(w, false);
    for (int i = 0; i < e->n; i++) if (!strcmp(e->items[i].name, text)) e->sel = i;
    layout(w);
    paint(w);
}

static void delete_done(int result, void *ctx)
{
    window_t *w = ctx;
    if (result != DLG_YES || !wm_window_alive(w)) return;
    struct explorer *e = w->app;
    int r = vfs_remove_tree(e->pending);
    if (r) {
        char m[160];
        snprintf(m, sizeof m, "Cannot delete: %s.", nbfs_strerror(r));
        msgbox(w, "Delete", m, ICON_ERROR, MB_OK, NULL, NULL);
    } else {
        klog("[expl] deleted %s\n", e->pending);
    }
    refresh(w);
}

static void start_rename(window_t *w)
{
    struct explorer *e = w->app;
    if (e->sel < 0) return;
    item_path(e, e->sel, e->pending);
    inputbox(w, "Rename", "Enter a new name:", e->items[e->sel].name, rename_done, w);
}

static void start_delete(window_t *w)
{
    struct explorer *e = w->app;
    if (e->sel < 0) return;
    item_path(e, e->sel, e->pending);
    char q[200];
    snprintf(q, sizeof q, "Are you sure you want to permanently delete \"%s\"%s?", e->items[e->sel].name,
             e->items[e->sel].type == NBFS_TYPE_DIR ? " and everything in it" : "");
    msgbox(w, "Delete", q, ICON_WARNING, MB_YESNO, delete_done, w);
}

static void create_new(window_t *w, bool folder)
{
    struct explorer *e = w->app;
    char p[NBFS_PATH_MAX];
    int r = vfs_create_unique(e->path, folder ? "New Folder" : "New Text Document", folder ? "" : ".txt",
                              folder ? NBFS_TYPE_DIR : NBFS_TYPE_FILE, p, sizeof p);
    if (r) {
        char m[120];
        snprintf(m, sizeof m, "Cannot create: %s.", nbfs_strerror(r));
        msgbox(w, "File Explorer", m, ICON_ERROR, MB_OK, NULL, NULL);
        return;
    }
    klog("[expl] created %s\n", p);
    load(w, false);
    for (int i = 0; i < e->n; i++) if (!strcmp(e->items[i].name, nbfs_path_basename(p))) e->sel = i;
    paint(w);
    start_rename(w);       /* let the user name it right away */
}

static void show_props(window_t *w)
{
    struct explorer *e = w->app;
    if (e->sel < 0) return;
    struct vfs_item *it = &e->items[e->sel];
    char sz[24], c[24], m[24], msg[400];
    vfs_format_size(it->size, sz, sizeof sz);
    vfs_format_time(it->ctime, c, sizeof c);
    vfs_format_time(it->mtime, m, sizeof m);
    snprintf(msg, sizeof msg, "Name: %s\nLocation: %s\nType: %s\nSize: %s\nCreated: %s\nModified: %s",
             it->name, e->path, it->type == NBFS_TYPE_DIR ? "Folder" : "File", sz, c, m);
    msgbox(w, "Properties", msg, icon_for_file(it->name, it->type == NBFS_TYPE_DIR), MB_OK, NULL, NULL);
}

static void menu_cb(int id, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    struct explorer *e = w->app;
    switch (id) {
    case M_OPEN: open_item(w, e->sel); break;
    case M_RENAME: start_rename(w); break;
    case M_DELETE: start_delete(w); break;
    case M_PROPS: show_props(w); break;
    case M_NEW_FOLDER: create_new(w, true); break;
    case M_NEW_TEXT: create_new(w, false); break;
    case M_REFRESH: refresh(w); break;
    case M_TERMINAL: terminal_open(e->path); break;
    }
}

static void ensure_visible(window_t *w)
{
    struct explorer *e = w->app;
    int rows = visible_rows(w);
    if (e->sel < e->scroll) e->scroll = e->sel;
    if (e->sel >= e->scroll + rows) e->scroll = e->sel - rows + 1;
    if (e->scroll < 0) e->scroll = 0;
}

static void on_event(window_t *w, event_t *ev)
{
    struct explorer *e = w->app;
    rect_t lr = list_rect(w), sr = scroll_rect(w);
    switch (ev->type) {
    case EV_PAINT: layout(w); paint(w); break;
    case EV_RESIZE: load(w, true); break;
    case EV_DROP: {
        /* onto a folder row: into that folder; anywhere else: into this folder */
        char dir[NBFS_PATH_MAX], out[NBFS_PATH_MAX];
        strlcpy(dir, e->path, sizeof dir);
        if (rect_contains(lr, ev->x, ev->y)) {
            int i = e->scroll + (ev->y - lr.y) / ROW_H;
            if (i < e->n && e->items[i].type == NBFS_TYPE_DIR) item_path(e, i, dir);
        }
        if (!strcmp(dir, ev->path)) break;     /* dropped onto itself */
        int r = vfs_move_into(ev->path, dir, ev->copy, out, sizeof out);
        if (r) {
            char m[200];
            snprintf(m, sizeof m, "Cannot %s \"%s\": %s.", ev->copy ? "copy" : "move", nbfs_path_basename(ev->path), nbfs_strerror(r));
            msgbox(w, "File Explorer", m, ICON_ERROR, MB_OK, NULL, NULL);
        } else klog("[expl] %s %s -> %s\n", ev->copy ? "copied" : "moved", ev->path, out);
        refresh(w);
        break;
    }
    case EV_WHEEL:
        e->scroll = MAX(0, MIN(MAX(0, e->n - visible_rows(w)), e->scroll + ev->wheel * 3));
        paint(w);
        break;
    case EV_TIMER: {
        /* pick up changes made elsewhere (terminal, desktop, other windows) */
        struct vfs_item *tmp;
        int n = vfs_list(e->path, &tmp);
        if (n < 0) { refresh(w); break; }
        u32 sig = signature_of(tmp, n);
        kfree(tmp);
        if (sig != e->signature) refresh(w);
        break;
    }
    case EV_MOUSE_MOVE: {
        if (e->drag_cand >= 0 && (ev->buttons & MOUSE_LEFT) &&
            ABS(ev->x - e->press_x) + ABS(ev->y - e->press_y) > 6) {
            char p[NBFS_PATH_MAX];
            item_path(e, e->drag_cand, p);
            int icon = icon_for_file(e->items[e->drag_cand].name, e->items[e->drag_cand].type == NBFS_TYPE_DIR);
            e->drag_cand = -1;
            wm_begin_dnd(p, icon);
            break;
        }
        if (e->drag_scroll) {
            e->scroll = ui_vscroll_offset_for(sr, e->n, visible_rows(w), ev->y);
            paint(w);
            break;
        }
        int h = ui_btn_hit(e->btn, NBTN, ev->x, ev->y);
        if (h != e->hover) { e->hover = h; paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        e->pressed = -1;
        if (ev->button == MOUSE_LEFT && (e->pressed = ui_btn_hit(e->btn, NBTN, ev->x, ev->y)) >= 0) { paint(w); break; }
        if (rect_contains(sr, ev->x, ev->y) && ev->button == MOUSE_LEFT) {
            e->drag_scroll = true;
            e->scroll = ui_vscroll_offset_for(sr, e->n, visible_rows(w), ev->y);
            paint(w);
            break;
        }
        if (ev->x < SIDEBAR_W && ev->y > TOOLBAR_H + 26 && ev->button == MOUSE_LEFT) {
            int i = (ev->y - TOOLBAR_H - 27) / 24;
            if (i >= 0 && i < (int)ARRAY_SIZE(places)) navigate(w, places[i].path, true);
            break;
        }
        if (rect_contains(lr, ev->x, ev->y)) {
            int i = e->scroll + (ev->y - lr.y) / ROW_H;
            e->sel = (i < e->n) ? i : -1;
            if (ev->button == MOUSE_LEFT && e->sel >= 0) { e->drag_cand = e->sel; e->press_x = ev->x; e->press_y = ev->y; }
            paint(w);
            if (ev->button == MOUSE_RIGHT) {
                rect_t c = wm_client_rect(w);
                if (e->sel >= 0) {
                    static const menu_item_t items[] = {
                        { "Open", M_OPEN, 0, -1 }, { "", 0, MENU_SEP, -1 },
                        { "Rename", M_RENAME, 0, -1 }, { "Delete", M_DELETE, 0, -1 }, { "", 0, MENU_SEP, -1 },
                        { "Properties", M_PROPS, 0, -1 },
                    };
                    menu_open(c.x + ev->x, c.y + ev->y, items, ARRAY_SIZE(items), menu_cb, w, false);
                } else {
                    static const menu_item_t items[] = {
                        { "New Folder", M_NEW_FOLDER, 0, ICON_FOLDER }, { "New Text File", M_NEW_TEXT, 0, ICON_FILE_TEXT },
                        { "", 0, MENU_SEP, -1 }, { "Refresh", M_REFRESH, 0, -1 },
                        { "Open Terminal Here", M_TERMINAL, 0, ICON_TERMINAL },
                    };
                    menu_open(c.x + ev->x, c.y + ev->y, items, ARRAY_SIZE(items), menu_cb, w, false);
                }
                klog("[expl] context menu\n");
            }
        }
        break;
    case EV_DBLCLICK:
        if (rect_contains(lr, ev->x, ev->y) && e->sel >= 0 && ev->button == MOUSE_LEFT) open_item(w, e->sel);
        break;
    case EV_MOUSE_UP: {
        e->drag_scroll = false;
        e->drag_cand = -1;
        int p = e->pressed;
        e->pressed = -1;
        if (p >= 0 && ui_btn_hit(e->btn, NBTN, ev->x, ev->y) == p) {
            if (p == B_BACK) go_back(w);
            else if (p == B_UP) go_up(w);
            else refresh(w);
        } else paint(w);
        break;
    }
    case EV_KEY: {
        struct key_event *k = &ev->key;
        if (!k->pressed) break;
        if (k->key == KEY_DOWN && e->n) { e->sel = MIN(e->n - 1, e->sel + 1); ensure_visible(w); paint(w); }
        else if (k->key == KEY_UP && e->n) { e->sel = MAX(0, e->sel - 1); ensure_visible(w); paint(w); }
        else if (k->key == KEY_ENTER) open_item(w, e->sel);
        else if (k->key == KEY_BACKSPACE) { if (e->nhist) go_back(w); else go_up(w); }
        else if (k->key == KEY_DELETE) start_delete(w);
        else if (k->key == KEY_F2) start_rename(w);
        else if (k->key == KEY_F5) refresh(w);
        else if (k->key == KEY_LEFT && (k->mods & MOD_ALT)) go_back(w);
        else if (k->key == KEY_UP && (k->mods & MOD_ALT)) go_up(w);
        break;
    }
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(e->items); kfree(e); break;
    }
}

void explorer_open(const char *path)
{
    struct explorer *e = kzalloc(sizeof *e);
    if (!e) return;
    strlcpy(e->path, path && vfs_is_dir(path) ? path : "/", sizeof e->path);
    e->sel = -1;
    e->drag_cand = -1;
    e->hover = e->pressed = -1;
    window_t *w = wm_create("File Explorer", ICON_EXPLORER, 640, 420, WF_RESIZABLE, on_event, e);
    if (!w) { kfree(e); return; }
    w->min_w = 400;
    w->min_h = 220;
    load(w, false);
    layout(w);
    paint(w);
    wm_set_timer(w, 2000);
}
