/* NimbusOS - Credits viewer backed by the root credits.txt file. */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gui/widgets.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"

#define LINE_HEIGHT 20

struct credits {
    char *text;
    u32 len;
    int error;
    int offset;
};

static window_t *credits_win;

static bool next_line(const char *text, u32 len, u32 *pos, int width, char *out, int cap)
{
    if (*pos >= len) return false;
    u32 start = *pos;
    if (text[start] == '\r' || text[start] == '\n') {
        if (text[start] == '\r') start++;
        if (start < len && text[start] == '\n') start++;
        *pos = start;
        if (out && cap) out[0] = 0;
        return true;
    }

    u32 end = start, last_space = len;
    while (end < len && text[end] != '\r' && text[end] != '\n') {
        if (gfx_ui_width_n(text + start, (int)(end - start + 1)) > width) {
            if (end == start) end++;
            else if (last_space != len) end = last_space;
            break;
        }
        if (text[end] == ' ') last_space = end;
        end++;
    }
    u32 count = end - start;
    if (out && cap) {
        u32 copy = MIN(count, (u32)cap - 1);
        memcpy(out, text + start, copy);
        out[copy] = 0;
    }
    *pos = end;
    while (*pos < len && (text[*pos] == ' ' || text[*pos] == '\t')) (*pos)++;
    if (*pos < len && text[*pos] == '\r') (*pos)++;
    if (*pos < len && text[*pos] == '\n') (*pos)++;
    return true;
}

static int line_count(const char *text, u32 len, int width)
{
    u32 pos = 0;
    int lines = 0;
    while (next_line(text, len, &pos, width, NULL, 0)) lines++;
    return lines;
}

static void paint(window_t *w)
{
    struct credits *c = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    rect_t page = rect_make(12, 12, s->w - 34, s->h - 48);
    int text_width = MAX(1, page.w - 16);
    int visible = MAX(1, (page.h - 12) / LINE_HEIGHT);

    gfx_fill_rect(s, page.x, page.y, page.w, page.h, 0xFFFFFF);
    gfx_rect(s, page.x, page.y, page.w, page.h, C_BORDER);
    if (c->error) {
        char message[96];
        snprintf(message, sizeof message, "Unable to load /credits.txt: %s", nbfs_strerror(c->error));
        gfx_ui_text(s, page.x + 8, page.y + 8, message, C_TEXT);
    } else {
        int total = line_count(c->text, c->len, text_width);
        c->offset = MAX(0, MIN(c->offset, total - visible));
        u32 pos = 0;
        char row[256];
        for (int i = 0; i < c->offset; i++) next_line(c->text, c->len, &pos, text_width, NULL, 0);
        gfx_set_clip(s, rect_make(page.x + 1, page.y + 1, page.w - 2, page.h - 2));
        for (int i = 0; i < visible && next_line(c->text, c->len, &pos, text_width, row, sizeof row); i++)
            gfx_ui_text(s, page.x + 8, page.y + 8 + i * LINE_HEIGHT, row, C_TEXT);
        gfx_reset_clip(s);
        ui_vscroll(s, rect_make(s->w - 18, page.y, 8, page.h), total, visible, c->offset);
    }
    gfx_ui_text(s, 12, s->h - 26, "Source: /credits.txt  |  Wheel or Up/Down to scroll", C_TEXT_DIM);
    wm_invalidate(w);
}

static void on_event(window_t *w, event_t *e)
{
    struct credits *c = w->app;
    switch (e->type) {
    case EV_PAINT:
    case EV_RESIZE:
        paint(w);
        break;
    case EV_WHEEL:
        c->offset = MAX(0, c->offset + e->wheel * 3);
        paint(w);
        break;
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ESC) { wm_destroy_window(w); break; }
        if (e->key.key == KEY_UP) c->offset = MAX(0, c->offset - 1);
        else if (e->key.key == KEY_DOWN) c->offset++;
        else if (e->key.key == KEY_PGUP) c->offset = MAX(0, c->offset - 8);
        else if (e->key.key == KEY_PGDN) c->offset += 8;
        else break;
        paint(w);
        break;
    case EV_CLOSE:
        wm_destroy_window(w);
        break;
    case EV_DESTROY:
        kfree(c->text);
        kfree(c);
        credits_win = NULL;
        break;
    }
}

void credits_open(const char *unused)
{
    if (wm_window_alive(credits_win)) {
        wm_focus(credits_win);
        return;
    }
    struct credits *c = kzalloc(sizeof *c);
    if (!c) {
        klog("[credits] cannot allocate viewer state\n");
        return;
    }
    c->error = vfs_read_all("/credits.txt", &c->text, &c->len);
    if (c->error) klog("[credits] cannot read /credits.txt: %s\n", nbfs_strerror(c->error));
    credits_win = wm_create("Credits", ICON_INFO, 540, 400, WF_RESIZABLE, on_event, c);
    if (!credits_win) {
        kfree(c->text);
        kfree(c);
        klog("[credits] cannot create window\n");
        return;
    }
    wm_center(credits_win);
}
