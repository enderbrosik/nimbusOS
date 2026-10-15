/* NimbusOS - widget drawing helpers. */
#include "gui/widgets.h"
#include "gui/theme.h"
#include "gui/icons.h"
#include "kernel/string.h"
#include "drivers/pit.h"

void ui_button(surface_t *s, rect_t r, const char *label, int state)
{
    u32 bg = C_BUTTON, border = C_BUTTON_BORDER, fg = C_TEXT;
    if (state == BTN_HOVER) { bg = C_BUTTON_HOVER; border = C_SELECT; }
    if (state == BTN_PRESSED) { bg = RGB(204, 228, 247); border = RGB(0, 84, 153); }
    if (state == BTN_DEFAULT) border = C_SELECT;
    if (state == BTN_DISABLED) { fg = C_TEXT_DIM; bg = RGB(235, 235, 235); }
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 3, border);
    gfx_round_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 2, bg);
    if (state == BTN_DEFAULT) gfx_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, C_SELECT);
    int off = state == BTN_PRESSED ? 1 : 0;
    rect_t t = r;
    t.x += off; t.y += off;
    gfx_text_centered(s, t, label, fg);
}

void ui_icon_button(surface_t *s, rect_t r, int icon, const char *label, int state)
{
    if (state == BTN_HOVER || state == BTN_PRESSED) {
        gfx_fill_rect(s, r.x, r.y, r.w, r.h, state == BTN_PRESSED ? RGB(204, 228, 247) : C_BUTTON_HOVER);
        gfx_rect(s, r.x, r.y, r.w, r.h, state == BTN_PRESSED ? RGB(0, 84, 153) : RGB(150, 190, 230));
    }
    u32 fg = state == BTN_DISABLED ? C_TEXT_DIM : C_TEXT;
    int x = r.x + 6;
    if (icon >= 0) { icon_draw_small(s, icon, x, r.y + (r.h - 16) / 2); x += 20; }
    if (label) {
        if (icon < 0) x = r.x + (r.w - gfx_text_width(label)) / 2;
        gfx_text(s, x, r.y + (r.h - 16) / 2, label, fg, COLOR_NONE);
    }
}

void ui_sunken(surface_t *s, rect_t r, u32 fill)
{
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, fill);
    gfx_rect(s, r.x, r.y, r.w, r.h, RGB(122, 122, 130));
}

void ui_checkbox(surface_t *s, int x, int y, bool checked, const char *label)
{
    gfx_fill_rect(s, x, y + 2, 13, 13, 0xFFFFFF);
    gfx_rect(s, x, y + 2, 13, 13, RGB(60, 60, 70));
    if (checked) {
        for (int i = 0; i < 2; i++) {
            gfx_line(s, x + 3, y + 8 + i, x + 5, y + 10 + i, C_SELECT);
            gfx_line(s, x + 5, y + 10 + i, x + 10, y + 5 + i, C_SELECT);
        }
    }
    if (label) gfx_text(s, x + 20, y, label, C_TEXT, COLOR_NONE);
}

void ui_radio(surface_t *s, int x, int y, bool checked, const char *label)
{
    gfx_fill_circle(s, x + 6, y + 8, 6, RGB(60, 60, 70));
    gfx_fill_circle(s, x + 6, y + 8, 5, 0xFFFFFF);
    if (checked) gfx_fill_circle(s, x + 6, y + 8, 3, C_SELECT);
    if (label) gfx_text(s, x + 20, y, label, C_TEXT, COLOR_NONE);
}

void ui_progress(surface_t *s, rect_t r, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ui_sunken(s, r, RGB(230, 230, 230));
    int w = (r.w - 2) * pct / 100;
    gfx_gradient_v(s, r.x + 1, r.y + 1, w, r.h - 2, RGB(80, 200, 100), RGB(20, 150, 50));
}

void ui_toolbar(surface_t *s, rect_t r)
{
    gfx_gradient_v(s, r.x, r.y, r.w, r.h, RGB(250, 250, 252), RGB(230, 232, 238));
    gfx_hline(s, r.x, r.y + r.h - 1, r.w, RGB(200, 200, 208));
}

void ui_separator_v(surface_t *s, int x, int y, int h)
{
    gfx_vline(s, x, y, h, RGB(200, 200, 208));
    gfx_vline(s, x + 1, y, h, 0xFFFFFF);
}

rect_t ui_vscroll(surface_t *s, rect_t r, int total, int visible, int offset)
{
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, RGB(236, 236, 238));
    gfx_vline(s, r.x, r.y, r.h, RGB(220, 220, 224));
    if (total <= visible || total <= 0) return rect_make(r.x, r.y, 0, 0);
    int th = r.h * visible / total;
    if (th < 20) th = 20;
    int range = total - visible;
    int ty = r.y + (r.h - th) * offset / range;
    rect_t t = rect_make(r.x + 2, ty, r.w - 4, th);
    gfx_round_rect(s, t.x, t.y, t.w, t.h, 3, RGB(180, 180, 186));
    return t;
}

int ui_vscroll_offset_for(rect_t r, int total, int visible, int thumb_y)
{
    if (total <= visible) return 0;
    int th = r.h * visible / total;
    if (th < 20) th = 20;
    int span = r.h - th;
    if (span <= 0) return 0;
    int off = (thumb_y - r.y) * (total - visible) / span;
    if (off < 0) off = 0;
    if (off > total - visible) off = total - visible;
    return off;
}

/* word-wrap helper; returns number of lines, draws if s != NULL */
static int wrap(surface_t *s, rect_t r, const char *text, u32 fg)
{
    int cols = r.w / FONT_W, line = 0;
    if (cols < 1) cols = 1;
    const char *p = text;
    while (*p) {
        int n = 0, last_space = -1;
        while (p[n] && p[n] != '\n' && n < cols) { if (p[n] == ' ') last_space = n; n++; }
        int take = n;
        if (p[n] && p[n] != '\n' && last_space > 0) take = last_space;
        if (s) gfx_text_n(s, r.x, r.y + line * (FONT_H + 2), p, take, fg, COLOR_NONE);
        line++;
        p += take;
        if (*p == ' ' || *p == '\n') p++;
    }
    return line;
}

void ui_text_wrapped(surface_t *s, rect_t r, const char *text, u32 fg) { wrap(s, r, text, fg); }
int ui_text_wrapped_height(const char *text, int width)
{
    return wrap(NULL, rect_make(0, 0, width, 0), text, 0) * (FONT_H + 2);
}

int ui_btn_hit(const ui_btn_t *b, int n, int x, int y)
{
    for (int i = 0; i < n; i++)
        if (!b[i].disabled && rect_contains(b[i].r, x, y)) return b[i].id;
    return -1;
}

/* ---- text field ---- */
void tf_set(textfield_t *tf, const char *s)
{
    strlcpy(tf->text, s, sizeof tf->text);
    tf->len = strlen(tf->text);
    tf->cursor = tf->len;
    tf->sel = 0;          /* select all, like most dialogs */
    tf->scroll = 0;
}

static void tf_delete_sel(textfield_t *tf)
{
    int a = MIN(tf->sel, tf->cursor), b = MAX(tf->sel, tf->cursor);
    if (a == b) return;
    memmove(tf->text + a, tf->text + b, tf->len - b + 1);
    tf->len -= b - a;
    tf->cursor = tf->sel = a;
}

bool tf_key(textfield_t *tf, struct key_event *ke)
{
    if (!ke->pressed) return false;
    bool shift = ke->mods & MOD_SHIFT;
    switch (ke->key) {
    case KEY_LEFT:  if (tf->cursor > 0) tf->cursor--; if (!shift) tf->sel = tf->cursor; return true;
    case KEY_RIGHT: if (tf->cursor < tf->len) tf->cursor++; if (!shift) tf->sel = tf->cursor; return true;
    case KEY_HOME:  tf->cursor = 0; if (!shift) tf->sel = 0; return true;
    case KEY_END:   tf->cursor = tf->len; if (!shift) tf->sel = tf->len; return true;
    case KEY_BACKSPACE:
        if (tf->sel != tf->cursor) tf_delete_sel(tf);
        else if (tf->cursor > 0) {
            memmove(tf->text + tf->cursor - 1, tf->text + tf->cursor, tf->len - tf->cursor + 1);
            tf->len--; tf->cursor--; tf->sel = tf->cursor;
        }
        return true;
    case KEY_DELETE:
        if (tf->sel != tf->cursor) tf_delete_sel(tf);
        else if (tf->cursor < tf->len) {
            memmove(tf->text + tf->cursor, tf->text + tf->cursor + 1, tf->len - tf->cursor);
            tf->len--;
        }
        return true;
    }
    if ((ke->mods & MOD_CTRL) && ke->key == 'a') { tf->sel = 0; tf->cursor = tf->len; return true; }
    if (ke->ch >= 32 && ke->ch < 127 && !(ke->mods & MOD_CTRL)) {
        tf_delete_sel(tf);
        if (tf->len >= (int)sizeof tf->text - 1) return true;
        memmove(tf->text + tf->cursor + 1, tf->text + tf->cursor, tf->len - tf->cursor + 1);
        tf->text[tf->cursor++] = ke->ch;
        tf->len++;
        tf->sel = tf->cursor;
        return true;
    }
    return false;
}

void tf_draw(surface_t *s, rect_t r, textfield_t *tf, bool focused)
{
    ui_sunken(s, r, 0xFFFFFF);
    if (focused) gfx_rect(s, r.x, r.y, r.w, r.h, C_SELECT);
    int cols = (r.w - 8) / FONT_W;
    if (tf->cursor < tf->scroll) tf->scroll = tf->cursor;
    if (tf->cursor > tf->scroll + cols) tf->scroll = tf->cursor - cols;
    int ty = r.y + (r.h - FONT_H) / 2;
    int a = MIN(tf->sel, tf->cursor), b = MAX(tf->sel, tf->cursor);
    rect_t old = s->clip;
    gfx_set_clip(s, rect_intersect(old, rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2)));
    for (int i = tf->scroll; i < tf->len && i <= tf->scroll + cols; i++) {
        int x = r.x + 4 + (i - tf->scroll) * FONT_W;
        bool sel = focused && i >= a && i < b;
        gfx_char(s, x, ty, tf->text[i], sel ? 0xFFFFFF : C_TEXT, sel ? C_SELECT : COLOR_NONE);
    }
    if (focused) gfx_vline(s, r.x + 4 + (tf->cursor - tf->scroll) * FONT_W, ty, FONT_H, C_TEXT);
    s->clip = old;
}

void tf_click(textfield_t *tf, rect_t r, int x)
{
    int pos = tf->scroll + (x - r.x - 4 + FONT_W / 2) / FONT_W;
    if (pos < 0) pos = 0;
    if (pos > tf->len) pos = tf->len;
    tf->cursor = tf->sel = pos;
}
