/*
 * NimbusOS - Snake.  Arrow keys (or WASD) steer, P/Space pauses, Enter
 * starts a new game.  The snake speeds up as it grows; the high score is
 * kept in /System/snake.hi.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define GW 32
#define GH 22
#define CELL 16
#define HUD 28
#define MAXLEN (GW * GH)
#define HISCORE_FILE "/System/snake.hi"

enum { READY, RUNNING, PAUSED, OVER };

struct snake {
    short x[MAXLEN], y[MAXLEN];
    int len, head;               /* ring buffer: body[head] is the head */
    int dx, dy, ndx, ndy;        /* current and queued direction */
    int fx, fy;                  /* food */
    int state, score, hiscore, speed_ms;
    bool grid[GH][GW];
};

static void load_hiscore(struct snake *g)
{
    char *d;
    if (vfs_read_all(HISCORE_FILE, &d, NULL) == 0) { g->hiscore = atoi(d); kfree(d); }
}

static void save_hiscore(struct snake *g)
{
    char b[16];
    int n = snprintf(b, sizeof b, "%d\n", g->hiscore);
    vfs_write_all(HISCORE_FILE, b, n);
}

static void place_food(struct snake *g)
{
    do { g->fx = rand_range(GW); g->fy = rand_range(GH); } while (g->grid[g->fy][g->fx]);
}

static void reset(struct snake *g)
{
    memset(g->grid, 0, sizeof g->grid);
    g->len = 4;
    g->head = g->len - 1;
    for (int i = 0; i < g->len; i++) {
        g->x[i] = 6 + i; g->y[i] = GH / 2;
        g->grid[g->y[i]][g->x[i]] = true;
    }
    g->dx = g->ndx = 1; g->dy = g->ndy = 0;
    g->score = 0;
    g->speed_ms = 130;
    g->state = READY;
    place_food(g);
}

static void paint(window_t *w)
{
    struct snake *g = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, HUD, RGB(30, 34, 44));
    char b[64];
    snprintf(b, sizeof b, "Score: %d", g->score);
    gfx_text_bold(s, 10, 6, b, 0xFFFFFF);
    snprintf(b, sizeof b, "Best: %d", g->hiscore);
    gfx_text(s, s->w - gfx_text_width(b) - 10, 6, b, RGB(255, 210, 80), COLOR_NONE);
    /* board with a subtle checkerboard */
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            gfx_fill_rect(s, x * CELL, HUD + y * CELL, CELL, CELL, ((x + y) & 1) ? RGB(28, 52, 34) : RGB(32, 58, 38));
    gfx_fill_circle(s, g->fx * CELL + 8, HUD + g->fy * CELL + 8, 6, RGB(235, 60, 50));
    gfx_fill_rect(s, g->fx * CELL + 8, HUD + g->fy * CELL, 2, 4, RGB(90, 60, 20));
    for (int i = 0; i < g->len; i++) {
        int idx = (g->head - i + MAXLEN) % MAXLEN;
        u32 c = i == 0 ? RGB(170, 255, 150) : color_blend(RGB(90, 220, 90), RGB(40, 140, 50), i * 255 / MAX(1, g->len));
        gfx_round_rect(s, g->x[idx] * CELL + 1, HUD + g->y[idx] * CELL + 1, CELL - 2, CELL - 2, 4, c);
        if (i == 0) {
            int ex = g->x[idx] * CELL + 8 + g->dx * 3, ey = HUD + g->y[idx] * CELL + 8 + g->dy * 3;
            gfx_fill_rect(s, ex - 3 * (g->dy != 0) - 1, ey - 3 * (g->dx != 0) - 1, 2, 2, 0);
            gfx_fill_rect(s, ex + 3 * (g->dy != 0) - 1, ey + 3 * (g->dx != 0) - 1, 2, 2, 0);
        }
    }
    const char *msg = g->state == READY ? "Press an arrow key to start" : g->state == PAUSED ? "Paused - press P"
                    : g->state == OVER ? "Game over - press Enter" : NULL;
    if (msg) {
        rect_t r = rect_make(s->w / 2 - 130, HUD + GH * CELL / 2 - 20, 260, 40);
        gfx_blend_rect(s, r.x, r.y, r.w, r.h, 0, 170);
        gfx_text_centered(s, r, msg, 0xFFFFFF);
    }
    wm_invalidate(w);
}

static void step(window_t *w)
{
    struct snake *g = w->app;
    if (g->state != RUNNING) return;
    /* reverse direction is ignored */
    if (!(g->ndx == -g->dx && g->ndy == -g->dy)) { g->dx = g->ndx; g->dy = g->ndy; }
    int hx = g->x[g->head] + g->dx, hy = g->y[g->head] + g->dy;
    int tail = (g->head - g->len + 1 + MAXLEN) % MAXLEN;
    bool eat = hx == g->fx && hy == g->fy;
    bool hit_wall = hx < 0 || hy < 0 || hx >= GW || hy >= GH;
    /* moving into the cell the tail is leaving is allowed */
    bool hit_self = !hit_wall && g->grid[hy][hx] && !(hx == g->x[tail] && hy == g->y[tail] && !eat);
    if (hit_wall || hit_self) {
        g->state = OVER;
        klog("[snak] game over, score %d\n", g->score);
        if (g->score > g->hiscore) { g->hiscore = g->score; save_hiscore(g); }
        return;
    }
    if (!eat) g->grid[g->y[tail]][g->x[tail]] = false;
    g->head = (g->head + 1) % MAXLEN;
    g->x[g->head] = hx; g->y[g->head] = hy;
    g->grid[hy][hx] = true;
    if (eat) {
        g->len++;
        g->score += 10;
        if (g->speed_ms > 60) g->speed_ms -= 4;
        wm_set_timer(w, g->speed_ms);
        klog("[snak] ate food, score %d\n", g->score);
        if (g->len < MAXLEN) place_food(g);
    }
}

static void on_event(window_t *w, event_t *e)
{
    struct snake *g = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: step(w); paint(w); break;
    case EV_BLUR: if (g->state == RUNNING) { g->state = PAUSED; paint(w); } break;
    case EV_KEY: {
        if (!e->key.pressed) break;
        int k = e->key.key, dx = 0, dy = 0;
        if (k == KEY_UP || k == 'w') dy = -1;
        else if (k == KEY_DOWN || k == 's') dy = 1;
        else if (k == KEY_LEFT || k == 'a') dx = -1;
        else if (k == KEY_RIGHT || k == 'd') dx = 1;
        if (dx || dy) {
            if (g->state == READY || g->state == PAUSED) g->state = RUNNING;
            if (g->state == RUNNING) { g->ndx = dx; g->ndy = dy; }
        } else if (k == 'p' || k == ' ') {
            if (g->state == RUNNING) g->state = PAUSED;
            else if (g->state == PAUSED) g->state = RUNNING;
        } else if (k == KEY_ENTER && g->state == OVER) {
            reset(g);
            wm_set_timer(w, g->speed_ms);
        }
        paint(w);
        break;
    }
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(g); break;
    }
}

void snake_open(const char *arg)
{
    struct snake *g = kzalloc(sizeof *g);
    if (!g) return;
    reset(g);
    load_hiscore(g);
    window_t *w = wm_create("Snake", ICON_SNAKE, GW * CELL, HUD + GH * CELL, 0, on_event, g);
    if (!w) { kfree(g); return; }
    w->mem_hint = sizeof *g;
    wm_set_timer(w, g->speed_ms);
}
