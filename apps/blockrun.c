/*
 * NimbusOS - Block Runner, an original side-scrolling jump game.
 *
 * Space, Up or a mouse click jumps.  Spikes end the run; blocks can be
 * landed on or jumped over.  The longer you survive, the faster it gets.
 * P pauses, Enter starts a new run.  The best distance is kept in
 * /System/blockrun.hi.
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
#include "kernel/sound.h"

#define W 480
#define H 270
#define HUD 26
#define FIELD_H (H - HUD)
#define GROUND_Y (FIELD_H - 36)     /* top of the ground, field coordinates */
#define PX 70                       /* the runner's fixed screen x */
#define PSIZE 24
#define GRAVITY 2
#define JUMP_V (-13)
#define TICK_MS 16
#define MAX_OBS 32
#define HISCORE_FILE "/System/blockrun.hi"

enum { READY, RUNNING, PAUSED, OVER };
enum { SPIKE, BLOCK };

struct obstacle { int kind, x, w, h; };      /* x is in world pixels */

struct runner {
    int state;
    int py, vy;                /* runner's top edge and vertical speed */
    bool on_ground;
    int world;                 /* distance travelled, world pixels */
    int speed;                 /* world pixels per tick */
    int next_spawn;            /* world x of the next obstacle to create */
    struct obstacle obs[MAX_OBS];
    int nobs;
    int score, hiscore;
};

static void load_hiscore(struct runner *g)
{
    char *d;
    if (vfs_read_all(HISCORE_FILE, &d, NULL) == 0) { g->hiscore = atoi(d); kfree(d); }
}

static void save_hiscore(struct runner *g)
{
    char b[16];
    int n = snprintf(b, sizeof b, "%d\n", g->hiscore);
    vfs_write_all(HISCORE_FILE, b, n);
}

/* Spikes and blocks, with a gap that narrows slowly as the run goes on. */
static void spawn_obstacle(struct runner *g)
{
    if (g->nobs >= MAX_OBS) return;
    struct obstacle *o = &g->obs[g->nobs++];
    o->x = g->next_spawn;
    if (rand_range(3) == 0) { o->kind = BLOCK; o->w = 30 + rand_range(3) * 10; o->h = 24 + rand_range(2) * 24; }
    else { o->kind = SPIKE; o->w = 26; o->h = 24; }
    int gap = 150 + rand_range(120) - MIN(60, g->world / 400);
    g->next_spawn = o->x + o->w + MAX(90, gap);
}

static void reset(struct runner *g)
{
    g->state = READY;
    g->py = GROUND_Y - PSIZE;
    g->vy = 0;
    g->on_ground = true;
    g->world = 0;
    g->speed = 4;
    g->nobs = 0;
    g->next_spawn = W + 120;
    g->score = 0;
    while (g->next_spawn < W * 3) spawn_obstacle(g);
}

static void paint(window_t *w)
{
    struct runner *g = w->app;
    surface_t *s = w->surface;
    /* sky: bands from light to dark, a sun, two slow hills */
    for (int i = 0; i < 6; i++)
        gfx_fill_rect(s, 0, HUD + i * FIELD_H / 6, s->w, FIELD_H / 6 + 1,
                      color_blend(RGB(120, 170, 240), RGB(40, 60, 130), i * 255 / 5));
    gfx_fill_circle(s, 400, HUD + 46, 22, RGB(255, 226, 120));
    int hill = (g->world / 4) % 240;
    for (int x = -hill; x < s->w + 240; x += 240) {
        gfx_fill_circle(s, x + 70, HUD + GROUND_Y + 10, 90, RGB(44, 80, 120));
        gfx_fill_circle(s, x + 190, HUD + GROUND_Y + 18, 70, RGB(36, 66, 104));
    }
    /* ground with stripes that scroll with the world */
    gfx_fill_rect(s, 0, HUD + GROUND_Y, s->w, FIELD_H - GROUND_Y, RGB(62, 46, 84));
    gfx_fill_rect(s, 0, HUD + GROUND_Y, s->w, 4, RGB(190, 110, 230));
    int stripe = g->world % 48;
    for (int x = -stripe; x < s->w; x += 48)
        gfx_fill_rect(s, x, HUD + GROUND_Y + 14, 24, 4, RGB(80, 60, 110));
    /* obstacles */
    for (int i = 0; i < g->nobs; i++) {
        struct obstacle *o = &g->obs[i];
        int sx = o->x - g->world;
        if (sx > s->w || sx + o->w < 0) continue;
        int top = HUD + GROUND_Y - o->h;
        if (o->kind == BLOCK) {
            gfx_round_rect(s, sx, top, o->w, o->h, 3, RGB(240, 176, 52));
            gfx_fill_rect(s, sx + 4, top + 4, o->w - 8, 4, RGB(255, 220, 130));
        } else {
            for (int r = 0; r < o->h; r++) {
                int half = (o->w / 2) * (o->h - r) / o->h;
                gfx_fill_rect(s, sx + o->w / 2 - half, top + r, half * 2 + 1, 1, RGB(230, 70, 80));
            }
        }
    }
    /* the runner: a rounded square with a face */
    int ry = HUD + g->py;
    gfx_round_rect(s, PX, ry, PSIZE, PSIZE, 5, RGB(90, 230, 170));
    gfx_fill_rect(s, PX + 6, ry + 7, 4, 4, 0x102030);
    gfx_fill_rect(s, PX + 14, ry + 7, 4, 4, 0x102030);
    gfx_fill_rect(s, PX + 6, ry + 16, 12, 2, 0x102030);
    /* HUD */
    gfx_fill_rect(s, 0, 0, s->w, HUD, RGB(22, 26, 40));
    char b[64];
    snprintf(b, sizeof b, "Distance: %d", g->score);
    gfx_text_bold(s, 10, 5, b, 0xFFFFFF);
    snprintf(b, sizeof b, "Best: %d", g->hiscore);
    gfx_text(s, s->w - gfx_text_width(b) - 10, 5, b, RGB(255, 210, 80), COLOR_NONE);
    const char *msg = g->state == READY ? "Press Space or click to start" : g->state == PAUSED ? "Paused - press P"
                    : g->state == OVER ? "Crashed - press Enter" : NULL;
    if (msg) {
        rect_t r = rect_make(s->w / 2 - 150, HUD + FIELD_H / 2 - 20, 300, 40);
        gfx_blend_rect(s, r.x, r.y, r.w, r.h, 0, 170);
        gfx_text_centered(s, r, msg, 0xFFFFFF);
    }
    wm_invalidate(w);
}

static void jump(struct runner *g)
{
    if (g->state == READY || g->state == PAUSED) g->state = RUNNING;
    if (g->state == RUNNING && g->on_ground) { g->vy = JUMP_V; g->on_ground = false; }
}

static void die(window_t *w, struct runner *g)
{
    g->state = OVER;
    sound_event(SND_BOOM);
    klog("[brun] crashed at distance %d\n", g->score);
    if (g->score > g->hiscore) { g->hiscore = g->score; save_hiscore(g); }
    paint(w);
}

static void tick(window_t *w)
{
    struct runner *g = w->app;
    if (g->state != RUNNING) return;
    g->speed = 4 + MIN(4, g->world / 1200);
    g->world += g->speed;
    g->score = g->world / 20;
    /* gravity and landing on the ground */
    int prev_bottom = g->py + PSIZE;
    g->vy += GRAVITY;
    g->py += g->vy;
    g->on_ground = false;
    if (g->py + PSIZE >= GROUND_Y) { g->py = GROUND_Y - PSIZE; g->vy = 0; g->on_ground = true; }
    /* obstacles: spikes are deadly, blocks can be landed on or hit from the side */
    for (int i = 0; i < g->nobs; i++) {
        struct obstacle *o = &g->obs[i];
        int sx = o->x - g->world;
        int top = GROUND_Y - o->h;
        bool overlap_x = PX + PSIZE > sx + 3 && PX < sx + o->w - 3;
        if (!overlap_x) continue;
        if (o->kind == SPIKE) {
            bool overlap_y = g->py + PSIZE > top + 6 && g->py < GROUND_Y - 4;
            if (overlap_y) { die(w, g); return; }
        } else {
            if (g->vy >= 0 && prev_bottom <= top + 2 && g->py + PSIZE >= top) {
                g->py = top - PSIZE; g->vy = 0; g->on_ground = true;
            } else if (g->py + PSIZE > top + 4 && g->py < GROUND_Y) {
                die(w, g); return;
            }
        }
    }
    /* forget obstacles that have scrolled away, and make new ones */
    int keep = 0;
    for (int i = 0; i < g->nobs; i++)
        if (g->obs[i].x - g->world + g->obs[i].w > -40) g->obs[keep++] = g->obs[i];
    g->nobs = keep;
    while (g->next_spawn - g->world < W * 2) spawn_obstacle(g);
}

static void on_event(window_t *w, event_t *e)
{
    struct runner *g = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: tick(w); paint(w); break;
    case EV_BLUR: if (g->state == RUNNING) { g->state = PAUSED; paint(w); } break;
    case EV_MOUSE_DOWN: jump(g); paint(w); break;
    case EV_KEY: {
        if (!e->key.pressed) break;
        int k = e->key.key;
        if (k == ' ' || k == KEY_UP || k == 'w') jump(g);
        else if (k == 'p') {
            if (g->state == RUNNING) g->state = PAUSED;
            else if (g->state == PAUSED) g->state = RUNNING;
        } else if (k == KEY_ENTER && g->state == OVER) {
            reset(g);
        }
        paint(w);
        break;
    }
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(g); break;
    }
}

void blockrun_open(const char *arg)
{
    struct runner *g = kzalloc(sizeof *g);
    if (!g) return;
    reset(g);
    load_hiscore(g);
    window_t *w = wm_create("Block Runner", ICON_BLOCKRUN, W, H, 0, on_event, g);
    if (!w) { kfree(g); return; }
    w->mem_hint = sizeof *g;
    wm_set_timer(w, TICK_MS);
}
