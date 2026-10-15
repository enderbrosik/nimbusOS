/*
 * NimbusOS - Task Manager: kernel tasks (CPU share, stack), application
 * windows (memory use, End Task) and overall memory / CPU history.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/task.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define HISTORY 60
#define ROW_H 20

struct taskmgr {
    u8 cpu_hist[HISTORY];
    int sel_win_id;
    rect_t end_btn;
    int end_state;
    rect_t app_rows[24];
    int app_ids[24];
    int napps;
};

static window_t *tm_win;
static const char *state_names[] = { "Running", "Sleeping", "Blocked", "Exited" };

static void paint(window_t *w)
{
    struct taskmgr *m = w->app;
    surface_t *s = w->surface;
    char b[128];
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);

    /* ---- kernel tasks ---- */
    struct task_info ti[24];
    int n = task_list(ti, 24);
    int idle = 0;
    for (int i = 0; i < n; i++) if (!strcmp(ti[i].name, "idle")) idle = ti[i].cpu_percent;
    gfx_text_bold(s, 12, 8, "Kernel tasks", C_TEXT);
    rect_t tr = rect_make(12, 28, s->w - 24, 22 + n * ROW_H);
    ui_sunken(s, tr, 0xFFFFFF);
    gfx_fill_rect(s, tr.x + 1, tr.y + 1, tr.w - 2, 20, RGB(240, 241, 245));
    gfx_text(s, tr.x + 8, tr.y + 3, "ID   Name             State        CPU    Stack", C_TEXT_DIM, COLOR_NONE);
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof b, "%-4d %-16s %-12s %3u%%   %3u KiB", ti[i].id, ti[i].name, state_names[ti[i].state],
                 ti[i].cpu_percent, ti[i].stack_size / 1024);
        gfx_text(s, tr.x + 8, tr.y + 23 + i * ROW_H, b, C_TEXT, COLOR_NONE);
    }

    /* ---- applications (windows) ---- */
    int ay = tr.y + tr.h + 14;
    gfx_text_bold(s, 12, ay, "Applications", C_TEXT);
    m->napps = 0;
    rect_t ar = rect_make(12, ay + 20, s->w - 24, 22 + 8 * ROW_H);
    ui_sunken(s, ar, 0xFFFFFF);
    gfx_fill_rect(s, ar.x + 1, ar.y + 1, ar.w - 2, 20, RGB(240, 241, 245));
    gfx_text(s, ar.x + 8, ar.y + 3, "Window                               Memory", C_TEXT_DIM, COLOR_NONE);
    bool sel_alive = false;
    for (int i = 0; i < wm_window_count() && m->napps < 8; i++) {
        window_t *x = wm_window_at(i);
        if (x->destroyed || (x->flags & WF_NO_TASKBAR)) continue;
        int y = ar.y + 22 + m->napps * ROW_H;
        rect_t row = rect_make(ar.x + 2, y, ar.w - 4, ROW_H);
        m->app_rows[m->napps] = row;
        m->app_ids[m->napps] = x->id;
        m->napps++;
        if (x->id == m->sel_win_id) { gfx_fill_rect(s, row.x, row.y, row.w, row.h, C_SELECT_LIGHT); sel_alive = true; }
        icon_draw_small(s, x->icon, row.x + 6, row.y + 2);
        u32 mem = (u32)x->surface->w * x->surface->h * 4 + x->mem_hint + sizeof(window_t);
        char title[40];
        strlcpy(title, x->title, sizeof title);
        snprintf(b, sizeof b, "%-34s %6u KiB%s", title, mem / 1024, x->minimized ? "  (minimized)" : "");
        gfx_text(s, row.x + 28, row.y + 2, b, C_TEXT, COLOR_NONE);
    }
    if (!sel_alive) m->sel_win_id = 0;

    /* ---- memory + CPU graph ---- */
    int gy = ar.y + ar.h + 12;
    struct heap_stats hs;
    heap_get_stats(&hs);
    u32 total = pmm_total_frames() * 4, freek = pmm_free_frames() * 4;
    snprintf(b, sizeof b, "Physical memory: %u MiB used of %u MiB", (total - freek) / 1024, total / 1024);
    gfx_text(s, 12, gy, b, C_TEXT, COLOR_NONE);
    ui_progress(s, rect_make(12, gy + 20, 260, 14), (int)((total - freek) * 100 / MAX(1u, total)));
    snprintf(b, sizeof b, "Kernel heap: %u KiB in use, %u KiB mapped", hs.used / 1024, hs.mapped / 1024);
    gfx_text(s, 12, gy + 42, b, C_TEXT, COLOR_NONE);
    snprintf(b, sizeof b, "Sampled by 'sysmon' task: %u times", g_sysmon.samples);
    gfx_text(s, 12, gy + 62, b, C_TEXT_DIM, COLOR_NONE);

    rect_t gr = rect_make(s->w - 12 - 2 * HISTORY - 2, gy, 2 * HISTORY + 2, 76);
    gfx_fill_rect(s, gr.x, gr.y, gr.w, gr.h, RGB(10, 24, 16));
    for (int y = gr.y + 19; y < gr.y + gr.h; y += 19) gfx_hline(s, gr.x, y, gr.w, RGB(20, 60, 36));
    for (int i = 1; i < HISTORY; i++) {
        int y0 = gr.y + gr.h - 2 - m->cpu_hist[i - 1] * (gr.h - 4) / 100;
        int y1 = gr.y + gr.h - 2 - m->cpu_hist[i] * (gr.h - 4) / 100;
        gfx_line(s, gr.x + 1 + (i - 1) * 2, y0, gr.x + 1 + i * 2, y1, RGB(80, 255, 120));
    }
    snprintf(b, sizeof b, "CPU %d%%", 100 - idle);
    gfx_text(s, gr.x, gr.y + gr.h + 2, b, C_TEXT, COLOR_NONE);

    m->end_btn = rect_make(s->w - 112, s->h - 38, 100, 28);
    ui_button(s, m->end_btn, "End Task", !m->sel_win_id ? BTN_DISABLED : m->end_state ? BTN_PRESSED : BTN_NORMAL);
    gfx_text(s, 12, s->h - 32, "Select an application and press End Task to close it.", C_TEXT_DIM, COLOR_NONE);
    wm_invalidate(w);
}

static void sample(struct taskmgr *m)
{
    struct task_info ti[24];
    int n = task_list(ti, 24), idle = 0;
    for (int i = 0; i < n; i++) if (!strcmp(ti[i].name, "idle")) idle = ti[i].cpu_percent;
    memmove(m->cpu_hist, m->cpu_hist + 1, HISTORY - 1);
    m->cpu_hist[HISTORY - 1] = (u8)MAX(0, MIN(100, 100 - idle));
}

static void on_event(window_t *w, event_t *e)
{
    struct taskmgr *m = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: sample(m); paint(w); break;
    case EV_MOUSE_DOWN:
        for (int i = 0; i < m->napps; i++) if (rect_contains(m->app_rows[i], e->x, e->y)) m->sel_win_id = m->app_ids[i];
        if (m->sel_win_id && rect_contains(m->end_btn, e->x, e->y)) m->end_state = 1;
        paint(w);
        break;
    case EV_MOUSE_UP:
        if (m->end_state && rect_contains(m->end_btn, e->x, e->y)) {
            for (int i = 0; i < wm_window_count(); i++) {
                window_t *x = wm_window_at(i);
                if (x->id == m->sel_win_id && !x->destroyed) {
                    klog("[tmgr] end task \"%s\"\n", x->title);
                    if (x == w) { m->end_state = 0; wm_destroy_window(w); return; }
                    wm_destroy_window(x);       /* forced: no save prompt */
                }
            }
            m->sel_win_id = 0;
        }
        m->end_state = 0;
        paint(w);
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(m); tm_win = NULL; break;
    }
}

void taskmgr_open(const char *unused)
{
    if (wm_window_alive(tm_win)) { wm_focus(tm_win); return; }
    struct taskmgr *m = kzalloc(sizeof *m);
    if (!m) return;
    tm_win = wm_create("Task Manager", ICON_TASKMGR, 560, 560, WF_NO_MAXIMIZE, on_event, m);
    if (!tm_win) { kfree(m); return; }
    wm_center(tm_win);
    wm_set_timer(tm_win, 1000);
}
