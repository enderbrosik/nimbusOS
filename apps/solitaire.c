/*
 * NimbusOS - Solitaire (Klondike).
 *
 * Drag cards (or runs of face-up cards) between piles; click the stock to
 * deal; double-click a card to send it to a foundation; Ctrl+Z / Undo goes
 * back.  Draw-1 or draw-3.  Winning launches the bouncing-cards finale.
 *
 * While dragging, the board without the dragged cards is cached in a
 * separate surface so each mouse move only redraws the area the cards left
 * and the area they now cover.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "kernel/heap.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define CW 71
#define CH 96
#define GAP 12
#define TOP_Y 12
#define TAB_Y 124
#define DOWN_OFF 6
#define UP_OFF 22
#define WIN_W (7 * (CW + GAP) + GAP)
#define WIN_H 580
#define BAR_H 30
#define UNDO_MAX 48
#define FELT RGB(0, 110, 50)

enum { P_STOCK, P_WASTE, P_F0, P_F1, P_F2, P_F3, P_T0, NPILES = P_T0 + 7 };

struct pile { u8 card[52]; bool up[52]; int n; };
struct state { struct pile p[NPILES]; int moves; };

struct bounce { int card; int x, y, vx, vy; };

struct sol {
    struct state s;
    struct state undo[UNDO_MAX];
    int nundo;
    int draw3;
    /* dragging */
    bool dragging;
    int from, from_idx, count;   /* source pile, first card index, number of cards */
    int grab_dx, grab_dy, dx, dy, press_x, press_y;
    bool moved;
    surface_t *bg;
    /* finale */
    bool won, finale;
    struct bounce b;
    int next_found, launched;
    rect_t btn_new, btn_draw, btn_undo;
};

static int suit(int c) { return c / 13; }
static int rank(int c) { return c % 13; }
static bool red(int c) { return suit(c) == 1 || suit(c) == 2; }

static int pile_x(int p)
{
    if (p == P_STOCK) return GAP;
    if (p == P_WASTE) return GAP + (CW + GAP);
    if (p < P_T0) return GAP + (3 + p - P_F0) * (CW + GAP);
    return GAP + (p - P_T0) * (CW + GAP);
}

static int card_y(struct sol *g, int p, int i)
{
    if (p < P_T0) return TOP_Y;
    int y = TAB_Y;
    for (int k = 0; k < i; k++) y += g->s.p[p].up[k] ? UP_OFF : DOWN_OFF;
    return y;
}

/* waste shows up to 3 fanned cards in draw-3 mode */
static int waste_x(struct sol *g, int i)
{
    int n = g->s.p[P_WASTE].n;
    int shown = g->draw3 ? MIN(3, n) : 1;
    int pos = i - (n - shown);
    return pile_x(P_WASTE) + (pos > 0 ? pos * 14 : 0);
}

static rect_t card_rect(struct sol *g, int p, int i)
{
    int x = p == P_WASTE ? waste_x(g, i) : pile_x(p);
    return rect_make(x, card_y(g, p, i), CW, CH);
}

/* ---------------------------------------------------------------- drawing */
static const char *rank_str(int r)
{
    static const char *n[13] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    return n[r];
}

static void draw_card(surface_t *s, int c, bool up, int x, int y)
{
    gfx_round_rect(s, x, y, CW, CH, 5, RGB(60, 60, 70));
    if (!up) {
        gfx_round_rect(s, x + 1, y + 1, CW - 2, CH - 2, 4, 0xFFFFFF);
        gfx_round_rect(s, x + 4, y + 4, CW - 8, CH - 8, 3, RGB(30, 70, 160));
        for (int j = y + 8; j < y + CH - 8; j += 6)
            for (int i = x + 8 + ((j / 6) & 1) * 3; i < x + CW - 8; i += 6)
                gfx_fill_rect(s, i, j, 2, 2, RGB(90, 140, 230));
        return;
    }
    gfx_round_rect(s, x + 1, y + 1, CW - 2, CH - 2, 4, 0xFFFFFF);
    u32 col = red(c) ? RGB(210, 20, 30) : RGB(20, 20, 24);
    char sym[2] = { (char)(3 + (suit(c) == 0 ? 3 : suit(c) == 1 ? 0 : suit(c) == 2 ? 1 : 2)), 0 };   /* CP437 suits */
    const char *rs = rank_str(rank(c));
    gfx_ui_text_bold(s, x + 5, y + 3, rs, col);
    gfx_text(s, x + 5, y + 19, sym, col, COLOR_NONE);
    gfx_ui_text_bold(s, x + CW - 6 - gfx_ui_width(rs), y + CH - 38, rs, col);
    gfx_text(s, x + CW - 13, y + CH - 21, sym, col, COLOR_NONE);
    if (rank(c) >= 10) {
        gfx_rect(s, x + 16, y + 20, CW - 32, CH - 40, col);
        gfx_text_scaled(s, x + CW / 2 - 8, y + CH / 2 - 16, rs, 2, col);
    } else {
        gfx_text_scaled(s, x + CW / 2 - 12, y + CH / 2 - 24, sym, 3, col);
    }
}

static void draw_slot(surface_t *s, int x, int y, const char *label)
{
    gfx_round_rect(s, x, y, CW, CH, 5, RGB(0, 80, 36));
    gfx_round_rect(s, x + 2, y + 2, CW - 4, CH - 4, 4, FELT);
    if (label) gfx_ui_centered(s, rect_make(x, y, CW, CH), label, RGB(0, 70, 30));
}

/* Draws the board; the cards being dragged are skipped when skip_drag. */
static void draw_board(struct sol *g, surface_t *s, bool skip_drag)
{
    gfx_fill_rect(s, 0, 0, s->w, s->h - BAR_H, FELT);
    for (int p = 0; p < NPILES; p++) {
        struct pile *pl = &g->s.p[p];
        int limit = pl->n;
        if (skip_drag && g->dragging && p == g->from) limit = g->from_idx;
        int first = 0;
        if (p < P_T0) first = MAX(0, limit - (p == P_WASTE && g->draw3 ? 3 : 2));   /* only top cards visible */
        if (limit == 0 || p == P_STOCK) {
            if (p == P_STOCK) {
                if (pl->n) draw_card(s, 0, false, pile_x(p), TOP_Y);
                else {
                    draw_slot(s, pile_x(p), TOP_Y, NULL);
                    gfx_circle(s, pile_x(p) + CW / 2, TOP_Y + CH / 2, 14, RGB(0, 70, 30));
                    gfx_circle(s, pile_x(p) + CW / 2, TOP_Y + CH / 2, 13, RGB(0, 70, 30));
                }
            } else if (limit == 0) draw_slot(s, pile_x(p), card_y(g, p, 0), p >= P_F0 && p <= P_F3 ? "A" : p >= P_T0 ? "K" : NULL);
            if (p == P_STOCK || limit == 0) continue;
        }
        for (int i = first; i < limit; i++) {
            rect_t r = card_rect(g, p, i);
            draw_card(s, pl->card[i], pl->up[i], r.x, r.y);
        }
    }
    int by = s->h - BAR_H;
    gfx_fill_rect(s, 0, by, s->w, BAR_H, RGB(236, 238, 243));
    gfx_hline(s, 0, by, s->w, RGB(200, 200, 208));
    g->btn_new = rect_make(6, by + 3, 92, BAR_H - 6);
    g->btn_draw = rect_make(104, by + 3, 92, BAR_H - 6);
    g->btn_undo = rect_make(202, by + 3, 70, BAR_H - 6);
    ui_button(s, g->btn_new, "New Game", BTN_NORMAL);
    ui_button(s, g->btn_draw, g->draw3 ? "Draw 3" : "Draw 1", BTN_NORMAL);
    ui_button(s, g->btn_undo, "Undo", g->nundo ? BTN_NORMAL : BTN_DISABLED);
    char st[64];
    int found = 0;
    for (int f = P_F0; f <= P_F3; f++) found += g->s.p[f].n;
    snprintf(st, sizeof st, "Moves: %d   Cards home: %d/52", g->s.moves, found);
    gfx_ui_text(s, s->w - gfx_ui_width(st) - 10, by + 7, st, C_TEXT);
}

static rect_t drag_rect(struct sol *g)
{
    int h = CH + (g->count - 1) * UP_OFF;
    return rect_make(g->dx - g->grab_dx, g->dy - g->grab_dy, CW, h);
}

static void draw_dragged(struct sol *g, surface_t *s)
{
    rect_t r = drag_rect(g);
    struct pile *pl = &g->s.p[g->from];
    for (int i = 0; i < g->count; i++) draw_card(s, pl->card[g->from_idx + i], true, r.x, r.y + i * UP_OFF);
}

static void paint(window_t *w)
{
    struct sol *g = w->app;
    if (g->finale) return;
    draw_board(g, w->surface, g->dragging);
    if (g->dragging) draw_dragged(g, w->surface);
    if (g->won) {
        rect_t r = rect_make(w->surface->w / 2 - 120, 300, 240, 40);
        gfx_blend_rect(w->surface, r.x, r.y, r.w, r.h, 0, 150);
        gfx_ui_centered(w->surface, r, "You win! Press New Game.", 0xFFFFFF);
    }
    wm_invalidate(w);
}

/* ---------------------------------------------------------------- rules */
static void push_undo(struct sol *g)
{
    if (g->nundo == UNDO_MAX) { memmove(&g->undo[0], &g->undo[1], (UNDO_MAX - 1) * sizeof g->undo[0]); g->nundo--; }
    g->undo[g->nundo++] = g->s;
}

static void deal(struct sol *g)
{
    u8 deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) { int j = rand_range(i + 1); u8 t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    memset(&g->s, 0, sizeof g->s);
    int k = 0;
    for (int t = 0; t < 7; t++)
        for (int i = 0; i <= t; i++) {
            struct pile *p = &g->s.p[P_T0 + t];
            p->card[p->n] = deck[k++];
            p->up[p->n] = i == t;
            p->n++;
        }
    struct pile *st = &g->s.p[P_STOCK];
    while (k < 52) st->card[st->n++] = deck[k++];
    g->nundo = 0;
    g->won = g->finale = false;
    klog("[sol ] new game (draw %d)\n", g->draw3 ? 3 : 1);
}

static bool can_drop(struct sol *g, int to, int card, int count)
{
    struct pile *p = &g->s.p[to];
    if (to >= P_F0 && to <= P_F3) {
        if (count != 1) return false;
        if (!p->n) return rank(card) == 0;
        int top = p->card[p->n - 1];
        return suit(top) == suit(card) && rank(card) == rank(top) + 1;
    }
    if (to >= P_T0) {
        if (!p->n) return rank(card) == 12;
        int top = p->card[p->n - 1];
        return p->up[p->n - 1] && red(top) != red(card) && rank(card) == rank(top) - 1;
    }
    return false;
}

static void check_win(struct sol *g)
{
    int found = 0;
    for (int f = P_F0; f <= P_F3; f++) found += g->s.p[f].n;
    if (found == 52 && !g->won) {
        g->won = true;
        klog("[sol ] won in %d moves\n", g->s.moves);
    }
}

static void move_cards(struct sol *g, int from, int idx, int to)
{
    struct pile *a = &g->s.p[from], *b = &g->s.p[to];
    int count = a->n - idx;
    for (int i = 0; i < count; i++) { b->card[b->n] = a->card[idx + i]; b->up[b->n] = true; b->n++; }
    a->n = idx;
    if (from >= P_T0 && a->n && !a->up[a->n - 1]) a->up[a->n - 1] = true;   /* auto-flip */
    g->s.moves++;
    check_win(g);
}

static bool auto_home(struct sol *g, int from)
{
    struct pile *a = &g->s.p[from];
    if (!a->n || !a->up[a->n - 1]) return false;
    int c = a->card[a->n - 1];
    for (int f = P_F0; f <= P_F3; f++)
        if (can_drop(g, f, c, 1)) { push_undo(g); move_cards(g, from, a->n - 1, f); return true; }
    return false;
}

static void click_stock(struct sol *g)
{
    klog("[sol ] stock (%d left)\n", g->s.p[P_STOCK].n);
    struct pile *st = &g->s.p[P_STOCK], *w = &g->s.p[P_WASTE];
    push_undo(g);
    if (!st->n) {                       /* recycle the waste */
        while (w->n) { st->card[st->n] = w->card[--w->n]; st->up[st->n++] = false; }
    } else {
        for (int i = 0; i < (g->draw3 ? 3 : 1) && st->n; i++) { w->card[w->n] = st->card[--st->n]; w->up[w->n++] = true; }
    }
    g->s.moves++;
}

/* Find the pile/card under a point. Returns pile or -1; *idx = card index or -1 (empty slot). */
static int hit(struct sol *g, int x, int y, int *idx)
{
    for (int p = 0; p < NPILES; p++) {
        struct pile *pl = &g->s.p[p];
        if (p == P_STOCK) {
            if (rect_contains(rect_make(pile_x(p), TOP_Y, CW, CH), x, y)) { *idx = pl->n - 1; return p; }
            continue;
        }
        for (int i = pl->n - 1; i >= 0; i--)
            if (rect_contains(card_rect(g, p, i), x, y)) { *idx = i; return p; }
        if (!pl->n && rect_contains(rect_make(pile_x(p), card_y(g, p, 0), CW, CH), x, y)) { *idx = -1; return p; }
    }
    return -1;
}

/* Drop target: the pile whose column/area overlaps the dragged cards the most. */
static int drop_target(struct sol *g)
{
    rect_t d = drag_rect(g);
    d.h = CH;
    int best = -1, best_area = 0;
    for (int p = P_F0; p < NPILES; p++) {
        if (p == g->from) continue;
        struct pile *pl = &g->s.p[p];
        rect_t r = pl->n ? card_rect(g, p, pl->n - 1) : rect_make(pile_x(p), card_y(g, p, 0), CW, CH);
        rect_t i = rect_intersect(d, r);
        if (i.w * i.h > best_area) { best_area = i.w * i.h; best = p; }
    }
    return best;
}

/* ---------------------------------------------------------------- finale */
static void finale_step(window_t *w)
{
    struct sol *g = w->app;
    surface_t *s = w->surface;
    if (!g->launched) {
        /* launch the next card from the foundations, cycling over them */
        int f = -1;
        for (int k = 0; k < 4; k++) {
            int c = P_F0 + (g->next_found + k) % 4;
            if (g->s.p[c].n) { f = c; g->next_found += k + 1; break; }
        }
        if (f < 0) { g->finale = false; wm_set_timer(w, 0); paint(w); return; }
        struct pile *p = &g->s.p[f];
        g->b.card = p->card[--p->n];
        g->b.x = pile_x(f) * 16;          /* fixed point, 1/16 pixel */
        g->b.y = TOP_Y * 16;
        g->b.vx = (rand_range(2) ? 1 : -1) * (40 + (int)rand_range(60));
        g->b.vy = -(int)rand_range(80);
        g->launched = 1;
    }
    int floor = (s->h - BAR_H - CH) * 16;
    for (int sub = 0; sub < 3; sub++) {
        g->b.vy += 12;
        g->b.x += g->b.vx;
        g->b.y += g->b.vy;
        if (g->b.y > floor) { g->b.y = floor; g->b.vy = -g->b.vy * 3 / 4; }
        int x = g->b.x / 16, y = g->b.y / 16;
        draw_card(s, g->b.card, true, x, y);   /* the trails stay on purpose */
        wm_invalidate_rect(w, rect_make(x - 8, y - 30, CW + 16, CH + 60));
    }
    if (g->b.x / 16 < -CW || g->b.x / 16 > s->w) g->launched = 0;
}

/* ---------------------------------------------------------------- events */
static void begin_drag(window_t *w, int p, int idx, int x, int y)
{
    struct sol *g = w->app;
    g->dragging = true;
    g->moved = false;
    g->from = p;
    g->from_idx = idx;
    g->count = g->s.p[p].n - idx;
    rect_t r = card_rect(g, p, idx);
    g->grab_dx = x - r.x;
    g->grab_dy = y - r.y;
    g->dx = x; g->dy = y;
    if (!g->bg) g->bg = surface_create(w->surface->w, w->surface->h);
    draw_board(g, g->bg, true);
}

static void on_event(window_t *w, event_t *e)
{
    struct sol *g = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: if (g->finale) finale_step(w); break;
    case EV_MOUSE_DOWN: {
        if (g->finale) { g->finale = false; wm_set_timer(w, 0); paint(w); break; }
        if (e->button != MOUSE_LEFT) break;
        g->press_x = e->x; g->press_y = e->y;
        if (rect_contains(g->btn_new, e->x, e->y)) { deal(g); paint(w); break; }
        if (rect_contains(g->btn_draw, e->x, e->y)) { g->draw3 = !g->draw3; deal(g); paint(w); break; }
        if (rect_contains(g->btn_undo, e->x, e->y)) {
            if (g->nundo) { g->s = g->undo[--g->nundo]; g->won = false; }
            paint(w);
            break;
        }
        int idx, p = hit(g, e->x, e->y, &idx);
        if (p < 0) break;
        if (p == P_STOCK) { click_stock(g); paint(w); break; }
        struct pile *pl = &g->s.p[p];
        if (idx < 0) break;
        if (p >= P_T0 && !pl->up[idx]) {
            if (idx == pl->n - 1) { push_undo(g); pl->up[idx] = true; g->s.moves++; paint(w); }
            break;
        }
        if (p == P_WASTE || (p >= P_F0 && p <= P_F3)) idx = pl->n - 1;   /* only the top card */
        begin_drag(w, p, idx, e->x, e->y);
        break;
    }
    case EV_MOUSE_MOVE:
        if (g->dragging) {
            rect_t old = drag_rect(g);
            g->dx = e->x; g->dy = e->y;
            if (ABS(e->x - g->press_x) + ABS(e->y - g->press_y) > 3) g->moved = true;
            rect_t nw = drag_rect(g);
            surface_t *s = w->surface;
            rect_t keep = s->clip;
            gfx_set_clip(s, old);
            gfx_blit(s, 0, 0, g->bg, 0, 0, s->w, s->h);
            s->clip = keep;
            draw_dragged(g, s);
            wm_invalidate_rect(w, rect_union(old, nw));
        }
        break;
    case EV_MOUSE_UP:
        if (g->dragging && e->button == MOUSE_LEFT) {
            g->dragging = false;
            if (g->moved) {
                int to = drop_target(g);
                int card = g->s.p[g->from].card[g->from_idx];
                if (to >= 0 && can_drop(g, to, card, g->count)) {
                    push_undo(g);
                    move_cards(g, g->from, g->from_idx, to);
                    klog("[sol ] moved %s to pile %d\n", rank_str(rank(card)), to);
                }
            }
            paint(w);
            if (g->won) { g->finale = true; g->launched = 0; g->next_found = 0; wm_set_timer(w, 30); }
        }
        break;
    case EV_DBLCLICK: {
        int idx, p = hit(g, e->x, e->y, &idx);
        if (p == P_WASTE || p >= P_T0) {
            g->dragging = false;
            if (auto_home(g, p)) klog("[sol ] auto-moved to foundation\n");
            paint(w);
            if (g->won) { g->finale = true; g->launched = 0; g->next_found = 0; wm_set_timer(w, 30); }
        }
        break;
    }
    case EV_KEY:
        if (!e->key.pressed) break;
        if ((e->key.mods & MOD_CTRL) && e->key.key == 'z' && g->nundo) { g->s = g->undo[--g->nundo]; g->won = false; paint(w); }
        else if (e->key.key == KEY_F2) { deal(g); paint(w); }
        else if ((e->key.mods & MOD_CTRL) && (e->key.mods & MOD_SHIFT) && e->key.key == 'f') {
            /* debug shortcut: send every card home to see the finale */
            for (int c = 0; c < 52; c++) {
                for (int p = 0; p < NPILES; p++) g->s.p[p].n = 0;
            }
            for (int c = 0; c < 52; c++) {
                struct pile *f = &g->s.p[P_F0 + suit(c)];
                f->card[f->n] = c; f->up[f->n] = true; f->n++;
            }
            check_win(g);
            paint(w);
            g->finale = true; g->launched = 0; g->next_found = 0;
            wm_set_timer(w, 30);
        }
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: surface_destroy(g->bg); kfree(g); break;
    }
}

void solitaire_open(const char *arg)
{
    struct sol *g = kzalloc(sizeof *g);
    if (!g) return;
    deal(g);
    window_t *w = wm_create("Solitaire", ICON_CARDS, WIN_W, WIN_H, 0, on_event, g);
    if (!w) { kfree(g); return; }
    w->mem_hint = sizeof *g + WIN_W * WIN_H * 4;
}
