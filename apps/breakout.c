/*
 * NimbusOS - Breakout, an original brick-breaking game.
 *
 * Move the paddle with the mouse, the arrow keys or A/D.  Space, Up or a
 * click launches the ball.  Clear a wall of bricks to get a faster one.
 * Three balls per game; P pauses, Enter starts a new game.  The best score
 * is kept in /System/breakout.hi.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/sound.h"

#define W 480
#define H 270
#define HUD 26
#define FIELD_H (H - HUD)
#define S 16                        /* ball positions are kept in 1/16 px */
#define BALL 6
#define PADDLE_W 72
#define PADDLE_H 8
#define PADDLE_Y (FIELD_H - 22)
#define PADDLE_STEP 8
#define COLS 10
#define ROWS 5
#define BRICK_W 42
#define BRICK_H 13
#define BRICK_X0 12                 /* left edge of the first brick column */
#define BRICK_STEP_X 46
#define BRICK_Y0 24
#define BRICK_STEP_Y 18
#define LIVES 3
#define BASE_SPEED (3 * S)          /* ball speed in 1/16 px per tick */
#define TICK_MS 16
#define HISCORE_FILE "/System/breakout.hi"
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

enum { READY, RUNNING, PAUSED, OVER };

struct breakout {
    int state;
    int bx, by, vx, vy;           /* ball top-left and velocity, 1/16 px */
    int paddle_x;                 /* paddle left edge, field px */
    int dir;                      /* -1 / 0 / 1 while a steering key is held */
    bool brick[ROWS * COLS];
    int bricks_left;
    int wall;                     /* walls cleared so far in this game */
    int lives, score, hiscore;
};

static void load_hiscore(struct breakout *g)
{
    char *d;
    if (vfs_read_all(HISCORE_FILE, &d, NULL) == 0) { g->hiscore = atoi(d); kfree(d); }
}

static void save_hiscore(struct breakout *g)
{
    char b[16];
    int n = snprintf(b, sizeof b, "%d\n", g->hiscore);
    vfs_write_all(HISCORE_FILE, b, n);
}

static int speed(struct breakout *g)
{
    return BASE_SPEED + MIN(8, g->wall) * 2;
}

/* Put the ball back on top of the paddle, waiting to be launched. */
static void park_ball(struct breakout *g)
{
    g->state = READY;
    g->bx = (g->paddle_x + PADDLE_W / 2 - BALL / 2) * S;
    g->by = (PADDLE_Y - BALL) * S;
    g->vx = 0;
    g->vy = 0;
}

static void fill_bricks(struct breakout *g)
{
    for (int i = 0; i < ROWS * COLS; i++) g->brick[i] = true;
    g->bricks_left = ROWS * COLS;
}

static void reset(struct breakout *g)
{
    g->wall = 0;
    g->lives = LIVES;
    g->score = 0;
    g->paddle_x = (W - PADDLE_W) / 2;
    g->dir = 0;
    fill_bricks(g);
    park_ball(g);
}

static void paint(window_t *w)
{
    struct breakout *g = w->app;
    surface_t *s = w->surface;
    gfx_gradient_v(s, 0, HUD, s->w, FIELD_H, RGB(18, 22, 40), RGB(40, 30, 64));
    /* bricks: the top rows are worth more */
    static const u32 row_col[ROWS] = { RGB(230, 70, 90), RGB(240, 150, 60), RGB(240, 210, 80),
                                       RGB(90, 200, 120), RGB(90, 150, 240) };
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            if (!g->brick[r * COLS + c]) continue;
            int x = BRICK_X0 + c * BRICK_STEP_X, y = HUD + BRICK_Y0 + r * BRICK_STEP_Y;
            gfx_round_rect(s, x, y, BRICK_W, BRICK_H, 2, row_col[r]);
            gfx_fill_rect(s, x + 3, y + 2, BRICK_W - 6, 2, color_lighten(row_col[r], 60));
        }
    /* paddle and ball */
    gfx_round_rect(s, g->paddle_x, HUD + PADDLE_Y, PADDLE_W, PADDLE_H, 4, RGB(200, 210, 230));
    gfx_fill_rect(s, g->paddle_x + 6, HUD + PADDLE_Y + 2, PADDLE_W - 12, 2, 0xFFFFFF);
    gfx_fill_circle(s, g->bx / S + BALL / 2, HUD + g->by / S + BALL / 2, BALL / 2 + 1, 0xFFFFFF);
    /* HUD */
    gfx_fill_rect(s, 0, 0, s->w, HUD, RGB(22, 26, 40));
    char b[64];
    snprintf(b, sizeof b, "Score: %d", g->score);
    gfx_text_bold(s, 10, 5, b, 0xFFFFFF);
    snprintf(b, sizeof b, "Lives: %d   Wall: %d   Best: %d", g->lives, g->wall + 1, g->hiscore);
    gfx_text(s, s->w - gfx_text_width(b) - 10, 5, b, RGB(255, 210, 80), COLOR_NONE);
    const char *msg = g->state == READY ? "Press Space or click to launch" : g->state == PAUSED ? "Paused - press P"
                    : g->state == OVER ? "Game over - press Enter" : NULL;
    if (msg) {
        rect_t r = rect_make(s->w / 2 - 150, HUD + FIELD_H / 2 - 20, 300, 40);
        gfx_blend_rect(s, r.x, r.y, r.w, r.h, 0, 170);
        gfx_text_centered(s, r, msg, 0xFFFFFF);
    }
    wm_invalidate(w);
}

static void launch(struct breakout *g)
{
    if (g->state == READY || g->state == PAUSED) {
        if (g->state == READY) { g->vx = speed(g) / 3; g->vy = -speed(g); }
        g->state = RUNNING;
    }
}

static void game_over(window_t *w, struct breakout *g)
{
    g->state = OVER;
    sound_event(SND_BOOM);
    klog("[brk ] game over, score %d\n", g->score);
    if (g->score > g->hiscore) { g->hiscore = g->score; save_hiscore(g); }
    paint(w);
}

/* A ball that reaches the bottom edge costs a life. */
static void lose_ball(window_t *w, struct breakout *g)
{
    g->lives--;
    if (g->lives <= 0) { game_over(w, g); return; }
    sound_event(SND_WARNING);
    klog("[brk ] ball lost, %d left\n", g->lives);
    park_ball(g);
}

/* Cleared the wall: start the next, faster one with the ball back on the paddle. */
static void next_wall(struct breakout *g)
{
    g->wall++;
    fill_bricks(g);
    sound_event(SND_WIN);
    klog("[brk ] cleared wall %d\n", g->wall);
    park_ball(g);
}

static void tick(window_t *w)
{
    struct breakout *g = w->app;
    if (g->state == READY && g->dir) {          /* the paddle still moves while the ball waits */
        g->paddle_x = CLAMP(g->paddle_x + g->dir * PADDLE_STEP, 0, W - PADDLE_W);
        park_ball(g);
    }
    if (g->state != RUNNING) return;
    if (g->dir) g->paddle_x = CLAMP(g->paddle_x + g->dir * PADDLE_STEP, 0, W - PADDLE_W);

    g->bx += g->vx;
    g->by += g->vy;
    int px = g->bx / S, py = g->by / S;
    /* side and top walls */
    if (px < 0) { g->bx = 0; g->vx = ABS(g->vx); }
    if (px + BALL > W) { g->bx = (W - BALL) * S; g->vx = -ABS(g->vx); }
    if (py < 0) { g->by = 0; g->vy = ABS(g->vy); }
    px = g->bx / S;
    py = g->by / S;
    /* paddle: the bounce angle depends on where the ball lands */
    if (g->vy > 0 && py + BALL >= PADDLE_Y && py + BALL < PADDLE_Y + PADDLE_H + 4 &&
        px + BALL > g->paddle_x && px < g->paddle_x + PADDLE_W) {
        int offset = (px + BALL / 2) - (g->paddle_x + PADDLE_W / 2);     /* -PADDLE_W/2 .. PADDLE_W/2 */
        int sp = speed(g);
        g->by = (PADDLE_Y - BALL) * S;
        g->vx = offset * sp / (PADDLE_W / 2);
        g->vy = -sp;
        sound_event(SND_CLICK);
    }
    /* bricks: one brick per tick, reflect off the side it was entered from */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int i = r * COLS + c;
            if (!g->brick[i]) continue;
            int bx = BRICK_X0 + c * BRICK_STEP_X, by = BRICK_Y0 + r * BRICK_STEP_Y;
            int ox = MIN(px + BALL, bx + BRICK_W) - MAX(px, bx);
            int oy = MIN(py + BALL, by + BRICK_H) - MAX(py, by);
            if (ox <= 0 || oy <= 0) continue;
            if (oy < ox) g->vy = (py + BALL / 2 < by + BRICK_H / 2) ? -ABS(g->vy) : ABS(g->vy);
            else         g->vx = (px + BALL / 2 < bx + BRICK_W / 2) ? -ABS(g->vx) : ABS(g->vx);
            g->brick[i] = false;
            g->bricks_left--;
            g->score += (ROWS - r) * 10;
            sound_event(SND_EAT);
            r = ROWS;                           /* leave both loops */
            break;
        }
    }
    if (g->bricks_left == 0) { next_wall(g); return; }
    if (g->by / S + BALL > FIELD_H) lose_ball(w, g);
}

static void on_event(window_t *w, event_t *e)
{
    struct breakout *g = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: tick(w); paint(w); break;
    case EV_BLUR: if (g->state == RUNNING) { g->state = PAUSED; paint(w); } break;
    case EV_MOUSE_MOVE:
        if (g->state == OVER) break;
        g->paddle_x = CLAMP(e->x - PADDLE_W / 2, 0, W - PADDLE_W);
        if (g->state == READY) park_ball(g);
        break;
    case EV_MOUSE_DOWN:
        if (g->state == OVER) reset(g);
        launch(g);
        paint(w);
        break;
    case EV_KEY: {
        int k = e->key.key;
        if (k == KEY_LEFT || k == 'a') g->dir = e->key.pressed ? -1 : (g->dir < 0 ? 0 : g->dir);
        else if (k == KEY_RIGHT || k == 'd') g->dir = e->key.pressed ? 1 : (g->dir > 0 ? 0 : g->dir);
        if (!e->key.pressed) break;
        if (k == ' ' || k == KEY_UP || k == 'w') launch(g);
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

void breakout_open(const char *arg)
{
    struct breakout *g = kzalloc(sizeof *g);
    if (!g) return;
    reset(g);
    load_hiscore(g);
    window_t *w = wm_create("Breakout", ICON_BREAKOUT, W, H, 0, on_event, g);
    if (!w) { kfree(g); return; }
    w->mem_hint = sizeof *g;
    wm_set_timer(w, TICK_MS);
}
