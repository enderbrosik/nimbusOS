/*
 * NimbusOS - Calculator.
 *
 * Four functions plus percent, sign change and backspace.  Operators apply
 * left to right (2 + 3 * 4 is 20), as on a basic desk calculator.  Keys can
 * be clicked or typed: Enter or = finishes, Backspace deletes, Esc clears.
 * The arithmetic is in apps/calc_math.h.
 */
#include "apps/apps.h"
#include "apps/calc_math.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define BW 56
#define BH 40
#define GAP 6
#define MARGIN 12
#define DISP_H 64
#define GRID_Y (MARGIN + DISP_H + 12)
#define CLIENT_W (2 * MARGIN + 4 * BW + 3 * GAP)
#define CLIENT_H (GRID_Y + 5 * BH + 4 * GAP + MARGIN)
#define NKEYS 20
#define CALC_INT_DIGITS 11                       /* whole digits typed before the limit */

/* Keys are the characters the arithmetic code sees: digits, . + - * / % = n (sign), b (backspace), c (clear). */
static const struct { const char *label; char key; } keys[NKEYS] = {
    { "C", 'c' },   { "Del", 'b' }, { "%", '%' },   { "/", '/' },
    { "7", '7' },   { "8", '8' },   { "9", '9' },   { "*", '*' },
    { "4", '4' },   { "5", '5' },   { "6", '6' },   { "-", '-' },
    { "1", '1' },   { "2", '2' },   { "3", '3' },   { "+", '+' },
    { "+/-", 'n' }, { "0", '0' },   { ".", '.' },   { "=", '=' },
};

struct calc {
    char entry[24];                 /* what the display shows: typed digits, or a result or message */
    bool fresh;                     /* the next digit starts a new number */
    bool error;                     /* entry holds a message such as "Overflow" */
    bool have_acc;                  /* acc and op are waiting for their right-hand side */
    calc_num acc;
    char op;
    int pressed;                    /* key under the mouse button, or -1 */
};

static rect_t key_rect(int i) { return rect_make(MARGIN + (i % 4) * (BW + GAP), GRID_Y + (i / 4) * (BH + GAP), BW, BH); }

static int key_at(int x, int y)
{
    for (int i = 0; i < NKEYS; i++) if (rect_contains(key_rect(i), x, y)) return i;
    return -1;
}

static void show(struct calc *c, calc_num v) { calc_format(v, c->entry, sizeof c->entry); }

static calc_num entry_value(struct calc *c)
{
    calc_num v = 0;
    calc_parse(c->entry, &v);
    return v;
}

static void clear_all(struct calc *c)
{
    strcpy(c->entry, "0");
    c->fresh = true;
    c->error = c->have_acc = false;
    c->acc = 0;
    c->op = 0;
}

static void fail(struct calc *c, const char *msg)
{
    snprintf(c->entry, sizeof c->entry, "%s", msg);
    c->error = true;
    c->have_acc = false;
    c->fresh = true;
    c->op = 0;
}

/* Applies the pending operator to the display.  Returns false (and shows the error) if it cannot. */
static bool settle(struct calc *c)
{
    calc_num r;
    int err = calc_apply(c->acc, c->op, entry_value(c), &r);
    if (err == CALC_DIV_ZERO) { fail(c, "Cannot divide by zero"); return false; }
    if (err == CALC_OVERFLOW) { fail(c, "Overflow"); return false; }
    c->acc = r;
    show(c, r);
    c->fresh = true;
    return true;
}

static void begin_entry(struct calc *c)
{
    if (c->fresh) { strcpy(c->entry, "0"); c->fresh = false; }
}

static void append(struct calc *c, char ch)
{
    int len = strlen(c->entry);
    c->entry[len] = ch;
    c->entry[len + 1] = 0;
}

static void press_digit(struct calc *c, char d)
{
    begin_entry(c);
    if (!strcmp(c->entry, "0")) c->entry[0] = 0;   /* a leading zero is replaced */
    int ints = 0, fracs = 0;
    bool dot = false;
    for (const char *p = c->entry; *p; p++) {
        if (*p == '.') dot = true;
        else if (*p >= '0' && *p <= '9') { if (dot) fracs++; else ints++; }
    }
    if (dot ? fracs >= CALC_DECIMALS : ints >= CALC_INT_DIGITS) return;
    append(c, d);
}

static void press_dot(struct calc *c)
{
    begin_entry(c);
    if (!strchr(c->entry, '.')) append(c, '.');
}

static void press_back(struct calc *c)
{
    if (c->fresh) return;
    int len = strlen(c->entry);
    c->entry[len - 1] = 0;
    if (!c->entry[0] || !strcmp(c->entry, "-")) strcpy(c->entry, "0");
}

static void press_neg(struct calc *c) { show(c, -entry_value(c)); }

static void press_percent(struct calc *c)
{
    calc_num r;
    if (calc_apply(entry_value(c), '/', 100 * CALC_SCALE, &r) == CALC_OK) { show(c, r); c->fresh = true; }
}

static void press_op(struct calc *c, char op)
{
    if (c->have_acc && !c->fresh) { if (!settle(c)) return; }
    else if (!c->have_acc) c->acc = entry_value(c);
    c->op = op;
    c->have_acc = true;
    c->fresh = true;
}

static void press_equals(struct calc *c)
{
    if (!c->have_acc) return;
    if (settle(c)) klog("[calc] = %s\n", c->entry);
    c->have_acc = false;
    c->op = 0;
}

static void press(struct calc *c, char key)
{
    if (c->error) clear_all(c);                 /* any key dismisses an error message */
    switch (key) {
    case 'c': clear_all(c); break;
    case 'b': press_back(c); break;
    case 'n': press_neg(c); break;
    case '%': press_percent(c); break;
    case '+': case '-': case '*': case '/': press_op(c, key); break;
    case '=': press_equals(c); break;
    case '.': press_dot(c); break;
    default: press_digit(c, key); break;        /* '0' to '9' */
    }
}

/* Printable keys come translated (Shift+= is '+'), the editing keys by their code. */
static char key_for(const struct key_event *k)
{
    char ch = k->ch;
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '=') return ch;
    if (k->key == KEY_ENTER) return '=';
    if (k->key == KEY_BACKSPACE) return 'b';
    if (k->key == KEY_ESC) return 'c';
    return 0;
}

/* ---- drawing ---- */
static void paint(window_t *w)
{
    struct calc *c = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, RGB(243, 243, 246));

    /* display: expression so far on top, the number (or message) large underneath */
    rect_t d = rect_make(MARGIN, MARGIN, s->w - 2 * MARGIN, DISP_H);
    gfx_round_rect(s, d.x, d.y, d.w, d.h, 6, 0xFFFFFF);
    if (c->have_acc) {
        char num[24], line[32];
        calc_format(c->acc, num, sizeof num);
        snprintf(line, sizeof line, "%s %c", num, c->op);
        gfx_ui_text(s, d.x + 12, d.y + 8, line, C_TEXT_DIM);
    }
    if (c->error) {
        gfx_ui_text_bold(s, d.x + 12, d.y + 30, c->entry, RGB(200, 30, 30));
    } else {
        int scale = 3;
        while (scale > 1 && gfx_text_width(c->entry) * scale > d.w - 24) scale--;
        int tw = gfx_text_width(c->entry) * scale;
        gfx_text_scaled(s, d.x + d.w - 12 - tw, d.y + 26, c->entry, scale, C_TEXT);
    }

    for (int i = 0; i < NKEYS; i++) {
        int state = i == c->pressed ? BTN_PRESSED : keys[i].key == '=' ? BTN_DEFAULT : BTN_NORMAL;
        ui_button(s, key_rect(i), keys[i].label, state);
    }
    wm_invalidate(w);
}

/* ---- events ---- */
static void on_event(window_t *w, event_t *e)
{
    struct calc *c = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_MOUSE_DOWN:
        if (e->button == MOUSE_LEFT) { c->pressed = key_at(e->x, e->y); paint(w); }
        break;
    case EV_MOUSE_UP:
        if (e->button == MOUSE_LEFT && c->pressed >= 0) {
            if (key_at(e->x, e->y) == c->pressed) press(c, keys[c->pressed].key);
            c->pressed = -1;
            paint(w);
        }
        break;
    case EV_KEY: {
        char k = e->key.pressed ? key_for(&e->key) : 0;
        if (k) { press(c, k); paint(w); }
        break;
    }
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(c); break;
    }
}

void calculator_open(const char *arg)
{
    struct calc *c = kzalloc(sizeof *c);
    if (!c) return;
    clear_all(c);
    c->pressed = -1;
    window_t *w = wm_create("Calculator", ICON_CALC, CLIENT_W, CLIENT_H, WF_NO_MAXIMIZE, on_event, c);
    if (!w) { kfree(c); return; }
    w->mem_hint = sizeof *c;
}
