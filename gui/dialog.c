/* NimbusOS - standard modal dialogs. */
#include "gui/dialog.h"
#include "gui/widgets.h"
#include "gui/theme.h"
#include "gui/icons.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

/* ======================================================================
 * Message box
 * ==================================================================== */
struct msgbox {
    msgbox_cb_t cb;
    void *ctx;
    char text[600];
    int icon;
    ui_btn_t btn[3];
    int nb, hover, pressed;
};

static void mb_paint(window_t *w)
{
    struct msgbox *m = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFF);
    gfx_fill_rect(s, 0, s->h - 48, s->w, 48, C_WINDOW_BG);
    gfx_hline(s, 0, s->h - 48, s->w, RGB(223, 223, 223));
    int tx = 20;
    if (m->icon >= 0) { icon_draw(s, m->icon, 20, 18); tx = 66; }
    ui_text_wrapped(s, rect_make(tx, 22, s->w - tx - 16, s->h - 70), m->text, C_TEXT);
    for (int i = 0; i < m->nb; i++)
        ui_button(s, m->btn[i].r, m->btn[i].label,
                  m->pressed == i ? BTN_PRESSED : m->hover == i ? BTN_HOVER : i == 0 ? BTN_DEFAULT : BTN_NORMAL);
    wm_invalidate(w);
}

static void mb_finish(window_t *w, int result)
{
    struct msgbox *m = w->app;
    msgbox_cb_t cb = m->cb;
    void *ctx = m->ctx;
    wm_destroy_window(w);
    if (cb) cb(result, ctx);
}

static void mb_event(window_t *w, event_t *e)
{
    struct msgbox *m = w->app;
    switch (e->type) {
    case EV_PAINT: mb_paint(w); break;
    case EV_MOUSE_MOVE: {
        int h = -1;
        for (int i = 0; i < m->nb; i++) if (rect_contains(m->btn[i].r, e->x, e->y)) h = i;
        if (h != m->hover) { m->hover = h; mb_paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        m->pressed = -1;
        for (int i = 0; i < m->nb; i++) if (rect_contains(m->btn[i].r, e->x, e->y)) m->pressed = i;
        mb_paint(w);
        break;
    case EV_MOUSE_UP: {
        int p = m->pressed;
        m->pressed = -1;
        if (p >= 0 && rect_contains(m->btn[p].r, e->x, e->y)) { mb_finish(w, m->btn[p].id); return; }
        mb_paint(w);
        break;
    }
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ENTER) mb_finish(w, m->btn[0].id);
        else if (e->key.key == KEY_ESC) mb_finish(w, m->btn[m->nb - 1].id);
        break;
    case EV_CLOSE: mb_finish(w, m->nb > 1 ? m->btn[m->nb - 1].id : DLG_OK); break;
    case EV_DESTROY: kfree(m); break;
    }
}

window_t *msgbox(window_t *owner, const char *title, const char *text, int icon, int buttons,
                 msgbox_cb_t cb, void *ctx)
{
    struct msgbox *m = kzalloc(sizeof *m);
    m->cb = cb; m->ctx = ctx; m->icon = icon; m->hover = m->pressed = -1;
    strlcpy(m->text, text, sizeof m->text);
    static const char *labels[4][3] = { { "OK" }, { "OK", "Cancel" }, { "Yes", "No" }, { "Yes", "No", "Cancel" } };
    static const int ids[4][3] = { { DLG_OK }, { DLG_OK, DLG_CANCEL }, { DLG_YES, DLG_NO }, { DLG_YES, DLG_NO, DLG_CANCEL } };
    static const int counts[4] = { 1, 2, 2, 3 };
    m->nb = counts[buttons];
    int w = 400;
    int th = ui_text_wrapped_height(text, w - (icon >= 0 ? 66 : 20) - 16);
    int h = MAX(th, 36) + 44 + 48;
    for (int i = 0; i < m->nb; i++) {
        m->btn[i].id = ids[buttons][i];
        m->btn[i].label = labels[buttons][i];
        m->btn[i].r = rect_make(w - (m->nb - i) * 92 - 4, h - 38, 84, 28);
    }
    window_t *win = wm_create(title, icon >= 0 ? icon : ICON_INFO, w, h, WF_DIALOG | WF_NO_TASKBAR, mb_event, m);
    if (!win) { kfree(m); return NULL; }
    wm_center(win);
    if (owner) wm_set_modal(owner, win);
    return win;
}

/* ======================================================================
 * Input box
 * ==================================================================== */
struct inputbox {
    input_cb_t cb;
    void *ctx;
    char prompt[160];
    textfield_t tf;
    ui_btn_t btn[2];
    int hover, pressed;
};

static void ib_paint(window_t *w)
{
    struct inputbox *b = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    gfx_ui_text(s, 16, 16, b->prompt, C_TEXT);
    tf_draw(s, rect_make(16, 40, s->w - 32, 26), &b->tf, true);
    for (int i = 0; i < 2; i++)
        ui_button(s, b->btn[i].r, b->btn[i].label,
                  b->pressed == i ? BTN_PRESSED : b->hover == i ? BTN_HOVER : i == 0 ? BTN_DEFAULT : BTN_NORMAL);
    wm_invalidate(w);
}

static void ib_finish(window_t *w, bool ok)
{
    struct inputbox *b = w->app;
    input_cb_t cb = b->cb;
    void *ctx = b->ctx;
    char text[256];
    strlcpy(text, b->tf.text, sizeof text);
    wm_destroy_window(w);
    if (cb) cb(ok, text, ctx);
}

static void ib_event(window_t *w, event_t *e)
{
    struct inputbox *b = w->app;
    switch (e->type) {
    case EV_PAINT: ib_paint(w); break;
    case EV_MOUSE_MOVE: {
        int h = ui_btn_hit(b->btn, 2, e->x, e->y);
        if (h != b->hover) { b->hover = h; ib_paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        b->pressed = ui_btn_hit(b->btn, 2, e->x, e->y);
        if (rect_contains(rect_make(16, 40, w->surface->w - 32, 26), e->x, e->y))
            tf_click(&b->tf, rect_make(16, 40, w->surface->w - 32, 26), e->x);
        ib_paint(w);
        break;
    case EV_MOUSE_UP: {
        int p = b->pressed;
        b->pressed = -1;
        if (p >= 0 && ui_btn_hit(b->btn, 2, e->x, e->y) == p) { ib_finish(w, p == 0); return; }
        ib_paint(w);
        break;
    }
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ENTER) { ib_finish(w, true); return; }
        if (e->key.key == KEY_ESC) { ib_finish(w, false); return; }
        if (tf_key(&b->tf, &e->key)) ib_paint(w);
        break;
    case EV_CLOSE: ib_finish(w, false); break;
    case EV_DESTROY: kfree(b); break;
    }
}

window_t *inputbox(window_t *owner, const char *title, const char *prompt, const char *initial,
                   input_cb_t cb, void *ctx)
{
    struct inputbox *b = kzalloc(sizeof *b);
    b->cb = cb; b->ctx = ctx; b->hover = b->pressed = -1;
    strlcpy(b->prompt, prompt, sizeof b->prompt);
    tf_set(&b->tf, initial ? initial : "");
    int w = 380, h = 124;
    b->btn[0] = (ui_btn_t){ rect_make(w - 188, h - 40, 84, 28), 0, "OK", -1, false };
    b->btn[1] = (ui_btn_t){ rect_make(w - 96, h - 40, 84, 28), 1, "Cancel", -1, false };
    window_t *win = wm_create(title, ICON_INFO, w, h, WF_DIALOG | WF_NO_TASKBAR, ib_event, b);
    if (!win) { kfree(b); return NULL; }
    wm_center(win);
    if (owner) wm_set_modal(owner, win);
    return win;
}

/* ======================================================================
 * File open / save dialog
 * ==================================================================== */
#define FD_ROW 20
struct filedlg {
    input_cb_t cb;
    void *ctx;
    int mode;
    char ext[16];
    char dir[NBFS_PATH_MAX];
    struct vfs_item *items;
    int n, sel, scroll;
    textfield_t name;
    ui_btn_t btn[3];        /* up, ok, cancel */
    int hover, pressed;
    bool focus_list;
};

static rect_t fd_list_rect(window_t *w) { return rect_make(12, 40, w->surface->w - 24, w->surface->h - 40 - 86); }

static bool fd_match(struct filedlg *f, struct vfs_item *it)
{
    if (it->type == NBFS_TYPE_DIR || !f->ext[0]) return true;
    const char *dot = strrchr(it->name, '.');
    return dot && !strcasecmp(dot, f->ext);
}

static void fd_load(struct filedlg *f)
{
    kfree(f->items);
    f->items = NULL;
    struct vfs_item *all;
    int n = vfs_list(f->dir, &all);
    f->n = 0;
    if (n > 0) {
        f->items = kmalloc(n * sizeof *all);
        for (int i = 0; i < n; i++) if (fd_match(f, &all[i])) f->items[f->n++] = all[i];
        kfree(all);
    }
    f->sel = -1;
    f->scroll = 0;
}

static void fd_paint(window_t *w)
{
    struct filedlg *f = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    gfx_ui_text(s, 12, 12, "Look in:", C_TEXT);
    ui_sunken(s, rect_make(82, 8, s->w - 82 - 50, 24), 0xFFFFFF);
    icon_draw_small(s, ICON_FOLDER, 86, 12);
    gfx_ui_text_ellipsis(s, 106, 12, f->dir, s->w - 106 - 56, C_TEXT);
    ui_icon_button(s, f->btn[0].r, ICON_NONE, "Up", f->pressed == 0 ? BTN_PRESSED : f->hover == 0 ? BTN_HOVER : BTN_NORMAL);
    gfx_rect(s, f->btn[0].r.x, f->btn[0].r.y, f->btn[0].r.w, f->btn[0].r.h, C_BUTTON_BORDER);

    rect_t lr = fd_list_rect(w);
    ui_sunken(s, lr, 0xFFFFFF);
    int rows = (lr.h - 4) / FD_ROW;
    rect_t old = s->clip;
    gfx_set_clip(s, rect_make(lr.x + 1, lr.y + 1, lr.w - 2, lr.h - 2));
    for (int i = 0; i < rows && f->scroll + i < f->n; i++) {
        struct vfs_item *it = &f->items[f->scroll + i];
        int y = lr.y + 2 + i * FD_ROW;
        bool sel = f->scroll + i == f->sel;
        if (sel) gfx_fill_rect(s, lr.x + 2, y, lr.w - 20, FD_ROW, C_SELECT_LIGHT);
        icon_draw_small(s, icon_for_file(it->name, it->type == NBFS_TYPE_DIR), lr.x + 6, y + 2);
        gfx_ui_text_ellipsis(s, lr.x + 28, y + 2, it->name, lr.w - 140, C_TEXT);
        if (it->type != NBFS_TYPE_DIR) {
            char sz[24];
            vfs_format_size(it->size, sz, sizeof sz);
            gfx_ui_text(s, lr.x + lr.w - 20 - gfx_ui_width(sz) - 6, y + 2, sz, C_TEXT_DIM);
        }
    }
    if (f->n == 0) gfx_ui_text(s, lr.x + 10, lr.y + 8, "(empty folder)", C_TEXT_DIM);
    s->clip = old;
    ui_vscroll(s, rect_make(lr.x + lr.w - 17, lr.y + 1, 16, lr.h - 2), f->n, rows, f->scroll);

    int by = s->h - 78;
    gfx_ui_text(s, 12, by + 5, "File name:", C_TEXT);
    tf_draw(s, rect_make(100, by, s->w - 112, 26), &f->name, !f->focus_list);
    if (f->ext[0]) {
        char t[48];
        snprintf(t, sizeof t, "Files of type: *%s", f->ext);
        gfx_ui_text(s, 100, by + 34, t, C_TEXT_DIM);
    }
    for (int i = 1; i < 3; i++)
        ui_button(s, f->btn[i].r, f->btn[i].label,
                  f->pressed == i ? BTN_PRESSED : f->hover == i ? BTN_HOVER : i == 1 ? BTN_DEFAULT : BTN_NORMAL);
    wm_invalidate(w);
}

static void fd_finish(window_t *w, bool ok, const char *path)
{
    struct filedlg *f = w->app;
    input_cb_t cb = f->cb;
    void *ctx = f->ctx;
    char p[NBFS_PATH_MAX];
    strlcpy(p, path ? path : "", sizeof p);
    wm_destroy_window(w);
    if (cb) cb(ok, p, ctx);
}

static void fd_overwrite_cb(int result, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    struct filedlg *f = w->app;
    if (result == DLG_YES) {
        char p[NBFS_PATH_MAX];
        nbfs_path_join(f->dir, f->name.text, p, sizeof p);
        fd_finish(w, true, p);
    }
}

static void fd_navigate(window_t *w, const char *dir)
{
    struct filedlg *f = w->app;
    strlcpy(f->dir, dir, sizeof f->dir);
    fd_load(f);
    fd_paint(w);
}

static void fd_accept(window_t *w)
{
    struct filedlg *f = w->app;
    if (!f->name.text[0]) {
        if (f->sel >= 0 && f->items[f->sel].type == NBFS_TYPE_DIR) {
            char p[NBFS_PATH_MAX];
            nbfs_path_join(f->dir, f->items[f->sel].name, p, sizeof p);
            fd_navigate(w, p);
        }
        return;
    }
    char p[NBFS_PATH_MAX];
    nbfs_path_join(f->dir, f->name.text, p, sizeof p);
    if (vfs_is_dir(p)) { tf_set(&f->name, ""); fd_navigate(w, p); return; }
    if (f->mode == FD_SAVE && f->ext[0]) {
        const char *dot = strrchr(f->name.text, '.');
        if (!dot || strcasecmp(dot, f->ext)) {
            strlcat(f->name.text, f->ext, sizeof f->name.text);
            f->name.len = strlen(f->name.text);
            nbfs_path_join(f->dir, f->name.text, p, sizeof p);
        }
    }
    if (f->mode == FD_OPEN && !vfs_exists(p)) {
        msgbox(w, "Open", "The file was not found. Check the file name and try again.", ICON_WARNING, MB_OK, NULL, NULL);
        return;
    }
    if (f->mode == FD_SAVE && vfs_exists(p)) {
        char q[160];
        snprintf(q, sizeof q, "%s already exists. Do you want to replace it?", f->name.text);
        msgbox(w, "Confirm Save As", q, ICON_WARNING, MB_YESNO, fd_overwrite_cb, w);
        return;
    }
    fd_finish(w, true, p);
}

static void fd_event(window_t *w, event_t *e)
{
    struct filedlg *f = w->app;
    rect_t lr = fd_list_rect(w);
    int rows = (lr.h - 4) / FD_ROW;
    switch (e->type) {
    case EV_PAINT: fd_paint(w); break;
    case EV_WHEEL:
        f->scroll = MAX(0, MIN(MAX(0, f->n - rows), f->scroll + e->wheel * 3));
        fd_paint(w);
        break;
    case EV_MOUSE_MOVE: {
        int h = ui_btn_hit(f->btn, 3, e->x, e->y);
        if (h != f->hover) { f->hover = h; fd_paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        f->pressed = ui_btn_hit(f->btn, 3, e->x, e->y);
        if (rect_contains(lr, e->x, e->y) && e->x < lr.x + lr.w - 17) {
            int i = f->scroll + (e->y - lr.y - 2) / FD_ROW;
            f->focus_list = true;
            if (i >= 0 && i < f->n) {
                f->sel = i;
                if (f->items[i].type != NBFS_TYPE_DIR) tf_set(&f->name, f->items[i].name);
            }
        } else if (e->x >= lr.x + lr.w - 17 && rect_contains(lr, e->x, e->y)) {
            f->scroll = ui_vscroll_offset_for(lr, f->n, rows, e->y);
        } else if (e->y > w->surface->h - 80 && e->y < w->surface->h - 52) {
            f->focus_list = false;
        }
        fd_paint(w);
        break;
    case EV_DBLCLICK:
        if (rect_contains(lr, e->x, e->y) && f->sel >= 0) {
            struct vfs_item *it = &f->items[f->sel];
            char p[NBFS_PATH_MAX];
            nbfs_path_join(f->dir, it->name, p, sizeof p);
            if (it->type == NBFS_TYPE_DIR) fd_navigate(w, p);
            else fd_accept(w);
        }
        break;
    case EV_MOUSE_UP: {
        int p = f->pressed;
        f->pressed = -1;
        if (p >= 0 && ui_btn_hit(f->btn, 3, e->x, e->y) == p) {
            if (p == 0) { char up[NBFS_PATH_MAX]; nbfs_path_parent(f->dir, up, sizeof up); fd_navigate(w, up); }
            else if (p == 1) fd_accept(w);
            else fd_finish(w, false, NULL);
            return;
        }
        fd_paint(w);
        break;
    }
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ENTER) { fd_accept(w); return; }
        if (e->key.key == KEY_ESC) { fd_finish(w, false, NULL); return; }
        if (e->key.key == KEY_DOWN || e->key.key == KEY_UP) {
            int d = e->key.key == KEY_DOWN ? 1 : -1;
            if (f->n) {
                f->sel = MAX(0, MIN(f->n - 1, f->sel + d));
                if (f->sel < f->scroll) f->scroll = f->sel;
                if (f->sel >= f->scroll + rows) f->scroll = f->sel - rows + 1;
                if (f->items[f->sel].type != NBFS_TYPE_DIR) tf_set(&f->name, f->items[f->sel].name);
                fd_paint(w);
            }
            break;
        }
        f->focus_list = false;
        if (tf_key(&f->name, &e->key)) fd_paint(w);
        break;
    case EV_CLOSE: fd_finish(w, false, NULL); break;
    case EV_DESTROY: kfree(f->items); kfree(f); break;
    }
}

window_t *filedialog(window_t *owner, int mode, const char *title, const char *start_dir,
                     const char *initial_name, const char *ext, input_cb_t cb, void *ctx)
{
    struct filedlg *f = kzalloc(sizeof *f);
    f->cb = cb; f->ctx = ctx; f->mode = mode; f->hover = f->pressed = -1;
    strlcpy(f->dir, start_dir && vfs_is_dir(start_dir) ? start_dir : "/", sizeof f->dir);
    if (ext) strlcpy(f->ext, ext, sizeof f->ext);
    tf_set(&f->name, initial_name ? initial_name : "");
    int w = 480, h = 360;
    f->btn[0] = (ui_btn_t){ rect_make(w - 44, 8, 34, 24), 0, "Up", -1, false };
    f->btn[1] = (ui_btn_t){ rect_make(w - 188, h - 40, 84, 28), 1, mode == FD_OPEN ? "Open" : "Save", -1, false };
    f->btn[2] = (ui_btn_t){ rect_make(w - 96, h - 40, 84, 28), 2, "Cancel", -1, false };
    fd_load(f);
    window_t *win = wm_create(title, mode == FD_OPEN ? ICON_FOLDER : ICON_FILE, w, h,
                              WF_DIALOG | WF_NO_TASKBAR, fd_event, f);
    if (!win) { kfree(f->items); kfree(f); return NULL; }
    wm_center(win);
    if (owner) wm_set_modal(owner, win);
    return win;
}
