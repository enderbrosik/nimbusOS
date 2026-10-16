/* NimbusOS - popup menus (context menus and the Start menu). */
#include "gui/menu.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gui/icons.h"
#include "kernel/string.h"

#define ITEM_H 26
#define SEP_H 9
#define BANNER_W 30

static struct {
    bool open, banner;
    rect_t r;
    int n, hover;
    menu_item_t items[MENU_MAX_ITEMS];
    char labels[MENU_MAX_ITEMS][48];
    menu_cb_t cb;
    void *ctx;
} m;

static int item_h(int i) { return (m.items[i].flags & MENU_SEP) ? SEP_H : ITEM_H; }

static void setup(const menu_item_t *items, int n, menu_cb_t cb, void *ctx, bool banner)
{
    if (m.open) wm_damage(m.r);
    if (n > MENU_MAX_ITEMS) n = MENU_MAX_ITEMS;
    m.n = n; m.cb = cb; m.ctx = ctx; m.banner = banner; m.hover = -1;
    int w = 140, h = 6;
    for (int i = 0; i < n; i++) {
        m.items[i] = items[i];
        strlcpy(m.labels[i], items[i].label ? items[i].label : "", sizeof m.labels[i]);
        m.items[i].label = m.labels[i];
        int tw = gfx_ui_width(m.labels[i]) + 60;
        if (tw > w) w = tw;
        h += item_h(i);
    }
    if (banner) w += BANNER_W;
    m.r.w = w; m.r.h = h;
}

static void place(int x, int y)
{
    if (x + m.r.w > SCREEN_W) x = SCREEN_W - m.r.w;
    if (y + m.r.h > SCREEN_H - TASKBAR_H) y = SCREEN_H - TASKBAR_H - m.r.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    m.r.x = x; m.r.y = y;
    m.open = true;
    wm_damage(m.r);
}

void menu_open(int x, int y, const menu_item_t *items, int n, menu_cb_t cb, void *ctx, bool banner)
{
    setup(items, n, cb, ctx, banner);
    place(x, y);
}

void menu_open_above(int x, int bottom_y, const menu_item_t *items, int n, menu_cb_t cb, void *ctx, bool banner)
{
    setup(items, n, cb, ctx, banner);
    m.r.x = x; m.r.y = bottom_y - m.r.h;
    if (m.r.y < 0) m.r.y = 0;
    m.open = true;
    wm_damage(m.r);
}

void menu_close(void)
{
    if (!m.open) return;
    m.open = false;
    wm_damage(m.r);
}

bool menu_is_open(void) { return m.open; }
bool menu_is_banner(void) { return m.open && m.banner; }
rect_t menu_rect(void) { return m.r; }

static int item_at(int x, int y)
{
    if (!m.open || !rect_contains(m.r, x, y)) return -1;
    int yy = m.r.y + 3;
    for (int i = 0; i < m.n; i++) {
        int h = item_h(i);
        if (y >= yy && y < yy + h) {
            if (m.items[i].flags & (MENU_SEP | MENU_DISABLED)) return -1;
            return i;
        }
        yy += h;
    }
    return -1;
}

void menu_draw(surface_t *s)
{
    if (!m.open || !rect_overlaps(m.r, s->clip)) return;
    rect_t r = m.r;
    gfx_blend_rect(s, r.x + 4, r.y + r.h, r.w, 4, 0, 60);
    gfx_blend_rect(s, r.x + r.w, r.y + 4, 4, r.h - 4, 0, 60);
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, C_MENU_BG);
    gfx_rect(s, r.x, r.y, r.w, r.h, C_MENU_BORDER);
    int x0 = r.x;
    if (m.banner) {
        gfx_gradient_v(s, r.x + 1, r.y + 1, BANNER_W, r.h - 2, RGB(20, 60, 130), RGB(0, 120, 215));
        const char *t = "NimbusOS";
        int n = strlen(t);
        for (int i = 0; i < n; i++)
            gfx_char(s, r.x + 1 + (BANNER_W - 8) / 2, r.y + r.h - 8 - (n - i) * 16, t[i], 0xFFFFFF, COLOR_NONE);
        x0 += BANNER_W;
    }
    int y = r.y + 3;
    for (int i = 0; i < m.n; i++) {
        int h = item_h(i);
        menu_item_t *it = &m.items[i];
        if (it->flags & MENU_SEP) {
            gfx_hline(s, x0 + 6, y + h / 2, r.x + r.w - x0 - 12, RGB(210, 210, 215));
        } else {
            u32 fg = (it->flags & MENU_DISABLED) ? C_TEXT_DIM : C_TEXT;
            if (i == m.hover) {
                gfx_fill_rect(s, x0 + 2, y, r.x + r.w - x0 - 4, h, C_SELECT);
                fg = 0xFFFFFF;
            }
            if (it->icon >= 0) icon_draw_small(s, it->icon, x0 + 8, y + (h - 16) / 2);
            gfx_ui_text(s, x0 + 32, y + (h - 16) / 2, it->label, fg);
        }
        y += h;
    }
}

bool menu_mouse_move(int x, int y)
{
    if (!m.open) return false;
    int h = item_at(x, y);
    if (h != m.hover) { m.hover = h; wm_damage(m.r); }
    return rect_contains(m.r, x, y);
}

bool menu_mouse_down(int x, int y)
{
    if (!m.open) return false;
    if (rect_contains(m.r, x, y)) return true;
    menu_close();
    return true;     /* a click outside closes the menu and is swallowed */
}

static void choose(int i)
{
    menu_cb_t cb = m.cb;
    void *ctx = m.ctx;
    int id = m.items[i].id;
    menu_close();
    if (cb) cb(id, ctx);
}

bool menu_mouse_up(int x, int y)
{
    if (!m.open) return false;
    int i = item_at(x, y);
    if (i >= 0) { choose(i); return true; }
    return rect_contains(m.r, x, y);
}

bool menu_key(struct key_event *ke)
{
    if (!m.open) return false;
    if (!ke->pressed) return true;
    int dir = 0;
    if (ke->key == KEY_ESC) { menu_close(); return true; }
    if (ke->key == KEY_DOWN) dir = 1;
    if (ke->key == KEY_UP) dir = -1;
    if (dir) {
        int i = (m.hover < 0 && dir < 0) ? 0 : m.hover;
        for (int k = 0; k < m.n; k++) {
            i = (i + dir + m.n) % m.n;
            if (!(m.items[i].flags & (MENU_SEP | MENU_DISABLED))) break;
        }
        m.hover = i;
        wm_damage(m.r);
    } else if (ke->key == KEY_ENTER && m.hover >= 0) {
        choose(m.hover);
    }
    return true;
}
