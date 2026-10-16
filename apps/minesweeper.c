/*
 * NimbusOS - Minesweeper.
 *
 * Left click reveals, right click cycles flag / question mark, clicking a
 * revealed number whose flags are all placed ("chording", left+right or
 * middle click or double-click) reveals its neighbours.  The first click is
 * always safe: mines are placed after it, away from the clicked cell.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "drivers/pit.h"
#include "kernel/heap.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/sound.h"

#define CELL 22
#define TOP 72
#define MARGIN 10
#define MAXW 30
#define MAXH 16

enum { HIDDEN, REVEALED, FLAGGED, QUESTION };
enum { PLAYING, WON, LOST };
static const struct { const char *name; int w, h, mines; } levels[3] = {
    { "Beginner", 9, 9, 10 }, { "Intermediate", 16, 16, 40 }, { "Expert", 30, 16, 99 },
};
static const u32 num_colors[9] = {
    0, RGB(25, 60, 220), RGB(20, 130, 30), RGB(210, 30, 30), RGB(20, 20, 120),
    RGB(120, 20, 20), RGB(20, 120, 120), RGB(0, 0, 0), RGB(120, 120, 120),
};

struct mines {
    int level, w, h, nmines;
    bool mine[MAXH][MAXW];
    u8 state[MAXH][MAXW];
    u8 count[MAXH][MAXW];
    bool placed;
    int status, flags, revealed;
    int exploded_x, exploded_y;
    u64 start_ms;
    int seconds;
    bool face_pressed;
    rect_t face, lvl_btn[3];
    u8 buttons_down;
};

static rect_t board_rect(struct mines *m) { return rect_make(MARGIN, TOP, m->w * CELL, m->h * CELL); }

static void client_size(int level, int *cw, int *ch)
{
    *cw = MAX(levels[level].w * CELL + 2 * MARGIN, 330);
    *ch = TOP + levels[level].h * CELL + MARGIN;
}

static void new_game(struct mines *m, int level)
{
    m->level = level;
    m->w = levels[level].w;
    m->h = levels[level].h;
    m->nmines = levels[level].mines;
    memset(m->mine, 0, sizeof m->mine);
    memset(m->state, 0, sizeof m->state);
    memset(m->count, 0, sizeof m->count);
    m->placed = false;
    m->status = PLAYING;
    m->flags = m->revealed = 0;
    m->seconds = 0;
    m->exploded_x = -1;
}

static void place_mines(struct mines *m, int sx, int sy)
{
    int left = m->nmines;
    while (left) {
        int x = rand_range(m->w), y = rand_range(m->h);
        if (m->mine[y][x] || (ABS(x - sx) <= 1 && ABS(y - sy) <= 1)) continue;
        m->mine[y][x] = true;
        left--;
    }
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if ((dx || dy) && nx >= 0 && ny >= 0 && nx < m->w && ny < m->h && m->mine[ny][nx]) m->count[y][x]++;
                }
    m->placed = true;
    m->start_ms = timer_ticks();
}

static void reveal(struct mines *m, int x, int y)
{
    /* iterative flood fill over empty cells */
    static short stack[MAXW * MAXH * 2];
    int n = 0;
    stack[n++] = x; stack[n++] = y;
    while (n) {
        int cy = stack[--n], cx = stack[--n];
        if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h) continue;
        if (m->state[cy][cx] != HIDDEN && m->state[cy][cx] != QUESTION) continue;
        if (m->mine[cy][cx]) {
            m->state[cy][cx] = REVEALED;
            m->status = LOST;
            m->exploded_x = cx; m->exploded_y = cy;
            continue;
        }
        m->state[cy][cx] = REVEALED;
        m->revealed++;
        if (m->count[cy][cx] == 0)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx || dy) { stack[n++] = cx + dx; stack[n++] = cy + dy; }
    }
    if (m->status == PLAYING && m->revealed == m->w * m->h - m->nmines) {
        m->status = WON;
        for (int yy = 0; yy < m->h; yy++)
            for (int xx = 0; xx < m->w; xx++)
                if (m->mine[yy][xx]) m->state[yy][xx] = FLAGGED;
        m->flags = m->nmines;
        klog("[mine] won %s in %d s\n", levels[m->level].name, m->seconds);
        sound_event(SND_WIN);
    }
    if (m->status == LOST) { klog("[mine] boom at %d,%d\n", m->exploded_x, m->exploded_y); sound_event(SND_BOOM); }
}

static void chord(struct mines *m, int x, int y)
{
    if (m->state[y][x] != REVEALED || !m->count[y][x]) return;
    int f = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < m->w && ny < m->h && m->state[ny][nx] == FLAGGED) f++;
        }
    if (f != m->count[y][x]) return;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (m->status == PLAYING) reveal(m, x + dx, y + dy);
}

/* ---- drawing ---- */
static void bevel(surface_t *s, rect_t r, bool raised, u32 face)
{
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, face);
    u32 hi = raised ? 0xFFFFFF : RGB(128, 128, 128), lo = raised ? RGB(128, 128, 128) : 0xFFFFFF;
    gfx_hline(s, r.x, r.y, r.w, hi); gfx_hline(s, r.x, r.y + 1, r.w - 1, hi);
    gfx_vline(s, r.x, r.y, r.h, hi); gfx_vline(s, r.x + 1, r.y, r.h - 1, hi);
    gfx_hline(s, r.x, r.y + r.h - 1, r.w, lo); gfx_hline(s, r.x + 1, r.y + r.h - 2, r.w - 1, lo);
    gfx_vline(s, r.x + r.w - 1, r.y, r.h, lo); gfx_vline(s, r.x + r.w - 2, r.y + 1, r.h - 1, lo);
}

static void led(surface_t *s, int x, int y, int v)
{
    char b[8];
    if (v < -99) v = -99;
    if (v > 999) v = 999;
    snprintf(b, sizeof b, "%03d", v);
    gfx_fill_rect(s, x, y, 56, 34, 0x000000);
    gfx_text_scaled(s, x + 4, y + 1, b, 2, RGB(255, 30, 30));
}

static void draw_mine(surface_t *s, int cx, int cy)
{
    gfx_fill_circle(s, cx, cy, 6, 0x000000);
    gfx_line(s, cx - 8, cy, cx + 8, cy, 0); gfx_line(s, cx, cy - 8, cx, cy + 8, 0);
    gfx_line(s, cx - 6, cy - 6, cx + 6, cy + 6, 0); gfx_line(s, cx - 6, cy + 6, cx + 6, cy - 6, 0);
    gfx_fill_rect(s, cx - 3, cy - 3, 2, 2, 0xFFFFFF);
}

static void draw_flag(surface_t *s, int x, int y)
{
    for (int i = 0; i < 6; i++) gfx_vline(s, x + 8 + i, y + 4 + i / 2, 9 - i, RGB(220, 0, 0));
    gfx_vline(s, x + 13, y + 4, 12, 0);
    gfx_fill_rect(s, x + 8, y + 15, 9, 2, 0);
    gfx_fill_rect(s, x + 6, y + 17, 13, 2, 0);
}

static void paint(window_t *w)
{
    struct mines *m = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, RGB(192, 192, 192));
    /* difficulty buttons */
    for (int i = 0; i < 3; i++) {
        m->lvl_btn[i] = rect_make(MARGIN + i * 104, 4, 100, 22);
        ui_button(s, m->lvl_btn[i], levels[i].name, i == m->level ? BTN_DEFAULT : BTN_NORMAL);
    }
    /* counter / face / timer panel */
    rect_t panel = rect_make(MARGIN, 30, s->w - 2 * MARGIN, 40);
    bevel(s, panel, false, RGB(192, 192, 192));
    led(s, panel.x + 4, panel.y + 3, m->nmines - m->flags);
    led(s, panel.x + panel.w - 60, panel.y + 3, m->seconds);
    m->face = rect_make(panel.x + (panel.w - 34) / 2, panel.y + 3, 34, 34);
    bevel(s, m->face, !m->face_pressed, RGB(192, 192, 192));
    int fx = m->face.x + 17, fy = m->face.y + 17;
    gfx_fill_circle(s, fx, fy, 12, RGB(255, 230, 0));
    gfx_circle(s, fx, fy, 12, 0);
    if (m->status == LOST) {
        gfx_line(s, fx - 7, fy - 6, fx - 3, fy - 2, 0); gfx_line(s, fx - 3, fy - 6, fx - 7, fy - 2, 0);
        gfx_line(s, fx + 3, fy - 6, fx + 7, fy - 2, 0); gfx_line(s, fx + 7, fy - 6, fx + 3, fy - 2, 0);
        gfx_circle(s, fx, fy + 6, 3, 0);
    } else if (m->status == WON) {
        gfx_fill_rect(s, fx - 9, fy - 5, 18, 4, 0);
        gfx_line(s, fx - 5, fy + 4, fx, fy + 7, 0); gfx_line(s, fx, fy + 7, fx + 5, fy + 4, 0);
    } else {
        gfx_fill_rect(s, fx - 5, fy - 5, 3, 3, 0); gfx_fill_rect(s, fx + 3, fy - 5, 3, 3, 0);
        if (m->buttons_down) gfx_circle(s, fx, fy + 5, 3, 0);
        else { gfx_line(s, fx - 5, fy + 4, fx, fy + 7, 0); gfx_line(s, fx, fy + 7, fx + 5, fy + 4, 0); }
    }
    /* board */
    rect_t b = board_rect(m);
    b.x = (s->w - b.w) / 2;
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++) {
            rect_t c = rect_make(b.x + x * CELL, b.y + y * CELL, CELL, CELL);
            u8 st = m->state[y][x];
            bool show_mine = m->status == LOST && m->mine[y][x] && st != FLAGGED;
            if (st == REVEALED || show_mine) {
                u32 face = (x == m->exploded_x && y == m->exploded_y) ? RGB(255, 0, 0) : RGB(192, 192, 192);
                gfx_fill_rect(s, c.x, c.y, c.w, c.h, face);
                gfx_hline(s, c.x, c.y, c.w, RGB(128, 128, 128));
                gfx_vline(s, c.x, c.y, c.h, RGB(128, 128, 128));
                if (m->mine[y][x]) draw_mine(s, c.x + 11, c.y + 11);
                else if (m->count[y][x]) {
                    char d[2] = { (char)('0' + m->count[y][x]), 0 };
                    gfx_text_bold(s, c.x + 7, c.y + 3, d, num_colors[m->count[y][x]]);
                }
            } else {
                bevel(s, c, true, RGB(192, 192, 192));
                if (st == FLAGGED) {
                    draw_flag(s, c.x, c.y);
                    if (m->status == LOST && !m->mine[y][x]) {   /* wrong flag */
                        gfx_line(s, c.x + 3, c.y + 3, c.x + 18, c.y + 18, RGB(220, 0, 0));
                        gfx_line(s, c.x + 18, c.y + 3, c.x + 3, c.y + 18, RGB(220, 0, 0));
                    }
                } else if (st == QUESTION) gfx_text_bold(s, c.x + 7, c.y + 3, "?", 0);
            }
        }
    wm_invalidate(w);
}

static void set_level(window_t *w, int level)
{
    struct mines *m = w->app;
    new_game(m, level);
    klog("[mine] level %s\n", levels[level].name);
    int cw, ch;
    client_size(level, &cw, &ch);
    rect_t f = w->frame;
    wm_set_frame(w, rect_make(f.x, f.y, cw + 2 * BORDER, ch + 2 * BORDER + TITLE_H));
    char t[48];
    snprintf(t, sizeof t, "Minesweeper - %s", levels[level].name);
    wm_set_title(w, t);
    paint(w);
}

static bool cell_at(window_t *w, int px, int py, int *cx, int *cy)
{
    struct mines *m = w->app;
    rect_t b = board_rect(m);
    b.x = (w->surface->w - b.w) / 2;
    if (!rect_contains(b, px, py)) return false;
    *cx = (px - b.x) / CELL;
    *cy = (py - b.y) / CELL;
    return true;
}

static void on_event(window_t *w, event_t *e)
{
    struct mines *m = w->app;
    int cx, cy;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER:
        if (m->status == PLAYING && m->placed) {
            int secs = (int)((timer_ticks() - m->start_ms) / 1000);
            if (secs != m->seconds) { m->seconds = secs; paint(w); }
        }
        break;
    case EV_MOUSE_DOWN:
        m->buttons_down = e->buttons;
        if (rect_contains(m->face, e->x, e->y)) m->face_pressed = true;
        for (int i = 0; i < 3; i++) if (rect_contains(m->lvl_btn[i], e->x, e->y)) { set_level(w, i); return; }
        paint(w);
        break;
    case EV_MOUSE_UP: {
        u8 held = m->buttons_down;
        m->buttons_down = e->buttons;
        if (m->face_pressed) {
            m->face_pressed = false;
            if (rect_contains(m->face, e->x, e->y)) { new_game(m, m->level); klog("[mine] new game\n"); }
            paint(w);
            break;
        }
        if (m->status != PLAYING || !cell_at(w, e->x, e->y, &cx, &cy)) { paint(w); break; }
        bool both = (held & (MOUSE_LEFT | MOUSE_RIGHT)) == (MOUSE_LEFT | MOUSE_RIGHT);
        if (both || e->button == MOUSE_MIDDLE) {
            chord(m, cx, cy);
            m->buttons_down = 0;
        } else if (e->button == MOUSE_LEFT) {
            if (m->state[cy][cx] == HIDDEN || m->state[cy][cx] == QUESTION) {
                bool first = !m->placed;
                if (first) place_mines(m, cx, cy);
                reveal(m, cx, cy);
                if (first) klog("[mine] first click %d,%d revealed %d cells\n", cx, cy, m->revealed);
            }
        } else if (e->button == MOUSE_RIGHT) {
            u8 *st = &m->state[cy][cx];
            if (*st == HIDDEN) { *st = FLAGGED; m->flags++; klog("[mine] flag %d,%d\n", cx, cy); }
            else if (*st == FLAGGED) { *st = QUESTION; m->flags--; }
            else if (*st == QUESTION) *st = HIDDEN;
        }
        paint(w);
        break;
    }
    case EV_DBLCLICK:
        if (m->status == PLAYING && e->button == MOUSE_LEFT && cell_at(w, e->x, e->y, &cx, &cy)) { chord(m, cx, cy); paint(w); }
        break;
    case EV_KEY:
        if (e->key.pressed && (e->key.key == KEY_F2 || e->key.key == 'n')) { new_game(m, m->level); paint(w); }
        else if (e->key.pressed && e->key.key >= '1' && e->key.key <= '3') set_level(w, e->key.key - '1');
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(m); break;
    }
}

void minesweeper_open(const char *arg)
{
    struct mines *m = kzalloc(sizeof *m);
    if (!m) return;
    new_game(m, 0);
    int cw, ch;
    client_size(0, &cw, &ch);
    window_t *w = wm_create("Minesweeper - Beginner", ICON_MINES, cw, ch, 0, on_event, m);
    if (!w) { kfree(m); return; }
    w->mem_hint = sizeof *m;
    wm_set_timer(w, 250);
}
