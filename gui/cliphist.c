/* NimbusOS - clipboard history popup (Ctrl+Shift+C). */
#include "gui/cliphist.h"
#include "gui/clipboard.h"
#include "gui/gfx.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "gui/widgets.h"
#include "kernel/heap.h"
#include "kernel/string.h"

#define ROW_H    26
#define LIST_Y   30
#define PREVIEW  60             /* characters of each entry shown */

struct cliphist {
    window_t *target;           /* window that gets the paste */
    int hover;                  /* row under the mouse, -1 if none */
    int sel;                    /* row highlighted by the arrow keys */
};

static window_t *popup;         /* the one open popup, if any */

static int rows(void) { return clipboard_history_count(); }

static rect_t row_rect(window_t *w, int i)
{
    return rect_make(8, LIST_Y + i * ROW_H, w->surface->w - 16, ROW_H - 2);
}

static void cl_paint(window_t *w)
{
    struct cliphist *c = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    gfx_ui_text(s, 10, 8, "Clipboard history", C_TEXT);
    if (!rows()) {
        gfx_ui_text(s, 12, LIST_Y + 6, "Nothing copied yet.", C_TEXT_DIM);
    }
    for (int i = 0; i < rows(); i++) {
        rect_t r = row_rect(w, i);
        bool lit = i == c->sel || i == c->hover;
        if (lit) gfx_fill_rect(s, r.x, r.y, r.w, r.h, C_SELECT);
        u32 len;
        const char *text = clipboard_history_get(i, &len);
        char line[PREVIEW + 4];
        int n = MIN((int)len, PREVIEW);
        line[0] = '1' + i;
        line[1] = ' ';
        line[2] = ' ';
        for (int k = 0; k < n; k++) {
            char ch = text[k];
            line[3 + k] = ch < 32 || ch >= 127 ? ' ' : ch;   /* one line per entry */
        }
        line[3 + n] = 0;
        gfx_ui_text_ellipsis(s, r.x + 6, r.y + 4, line, r.w - 12, lit ? 0xFFFFFF : C_TEXT);
    }
    gfx_ui_text(s, 10, s->h - 22, "1-5 or Enter: paste    Esc: close", C_TEXT_DIM);
    wm_invalidate(w);
}

/* Make entry i the current clipboard, then paste it where the popup was opened. */
static void pick(window_t *w, int i)
{
    struct cliphist *c = w->app;
    u32 len;
    const char *text = clipboard_history_get(i, &len);
    if (!text) return;
    clipboard_set(text, len);
    window_t *target = c->target;
    wm_destroy_window(w);                     /* focus returns to the target */
    if (target && wm_window_alive(target)) {
        struct key_event paste = { 'v', 'v', 1, MOD_CTRL };
        wm_post_key(target, &paste);
    }
}

static void cl_event(window_t *w, event_t *e)
{
    struct cliphist *c = w->app;
    switch (e->type) {
    case EV_PAINT: cl_paint(w); break;
    case EV_MOUSE_MOVE: {
        int h = -1;
        for (int i = 0; i < rows() && h < 0; i++)
            if (rect_contains(row_rect(w, i), e->x, e->y)) h = i;
        if (h != c->hover) { c->hover = h; cl_paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        for (int i = 0; i < rows(); i++)
            if (rect_contains(row_rect(w, i), e->x, e->y)) { pick(w, i); return; }
        break;
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ESC) { wm_destroy_window(w); return; }
        if (e->key.key == KEY_ENTER && rows()) { pick(w, c->sel); return; }
        if (e->key.key == KEY_UP || e->key.key == KEY_DOWN) {
            int d = e->key.key == KEY_DOWN ? 1 : -1;
            if (rows()) c->sel = MAX(0, MIN(rows() - 1, c->sel + d));
            cl_paint(w);
            break;
        }
        if (e->key.ch >= '1' && e->key.ch < '1' + rows()) pick(w, e->key.ch - '1');
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY:
        if (popup == w) popup = NULL;
        kfree(c);
        break;
    }
}

void cliphist_open(window_t *target)
{
    if (wm_window_alive(popup)) { wm_focus(popup); return; }
    struct cliphist *c = kzalloc(sizeof *c);
    if (!c) return;
    c->target = target;
    c->hover = -1;
    int w = 300, h = LIST_Y + CLIP_HISTORY * ROW_H + 30;
    popup = wm_create("Clipboard", ICON_INFO, w, h, WF_DIALOG | WF_NO_TASKBAR, cl_event, c);
    if (!popup) { kfree(c); return; }
    wm_center(popup);
    if (target) wm_set_modal(target, popup);
}
