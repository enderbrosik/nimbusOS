/*
 * NimbusOS - taskbar: Start button + Start menu, one button per window,
 * a system tray and a clock.  The clock reads the CMOS RTC at boot and every
 * minute, and advances with the PIT tick count in between.
 */
#include "gui/taskbar.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gui/menu.h"
#include "gui/icons.h"
#include "gui/dialog.h"
#include "apps/apps.h"
#include "fs/vfs.h"
#include "kernel/power.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/string.h"
#include "kernel/settings.h"
#include "kernel/kprintf.h"
#include "drivers/pit.h"
#include "drivers/fb.h"

#define BTN_X0 98
#define CLOCK_W 92
#define TRAY_W 52
#define MAX_BTNS 32

static rect_t bar;
static rect_t start_btn;
static window_t *btn_win[MAX_BTNS];
static rect_t btn_rect[MAX_BTNS];
static int nbtn;
static int hover = -2;              /* -1 = start button, >=0 window button */
static u32 base_time;
static u64 base_ticks;
static int last_minute = -1;
static u32 mem_used_pct;

enum { SM_APP = 100, SM_PROPS = 200, SM_RESTART, SM_SHUTDOWN };
enum { WM_RESTORE = 300, WM_MINIMIZE, WM_MAXIMIZE, WM_CLOSE };

/* The RTC keeps UTC; the time zone setting is an offset in minutes. */
void clock_now(struct rtc_time *t)
{
    u32 now = base_time + (u32)((timer_ticks() - base_ticks) / 1000) + settings_get_int("tz_minutes", 0) * 60;
    unix_to_rtc(now, t);
}

static void resync_clock(void)
{
    base_time = rtc_unix_time();
    base_ticks = timer_ticks();
}

void taskbar_init(void)
{
    bar = rect_make(0, SCREEN_H - TASKBAR_H, SCREEN_W, TASKBAR_H);
    start_btn = rect_make(3, bar.y + 3, 88, TASKBAR_H - 6);
    resync_clock();
}

rect_t taskbar_rect(void) { return bar; }
rect_t taskbar_start_rect(void) { return start_btn; }
void taskbar_changed(void) { wm_damage(bar); }

static void layout(void)
{
    nbtn = 0;
    /* stable order: by window id (creation order) */
    int last_id = 0;
    for (;;) {
        window_t *best = NULL;
        for (int i = 0; i < wm_window_count(); i++) {
            window_t *w = wm_window_at(i);
            if (w->destroyed || (w->flags & WF_NO_TASKBAR) || w->id <= last_id) continue;
            if (!best || w->id < best->id) best = w;
        }
        if (!best || nbtn == MAX_BTNS) break;
        btn_win[nbtn++] = best;
        last_id = best->id;
    }
    int avail = SCREEN_W - CLOCK_W - TRAY_W - BTN_X0 - 8;
    int bw = nbtn ? MIN(180, avail / nbtn) : 0;
    for (int i = 0; i < nbtn; i++) btn_rect[i] = rect_make(BTN_X0 + i * bw, bar.y + 3, bw - 4, TASKBAR_H - 6);
}

static void draw_start(surface_t *s)
{
    bool open = menu_is_banner();
    u32 c1 = open ? RGB(20, 110, 60) : hover == -1 ? RGB(60, 190, 100) : RGB(40, 160, 80);
    u32 c2 = open ? RGB(10, 80, 40) : hover == -1 ? RGB(30, 140, 70) : RGB(20, 120, 55);
    gfx_round_rect(s, start_btn.x, start_btn.y, start_btn.w, start_btn.h, 5, RGB(10, 60, 30));
    rect_t in = rect_make(start_btn.x + 1, start_btn.y + 1, start_btn.w - 2, start_btn.h - 2);
    gfx_round_rect(s, in.x, in.y, in.w, in.h, 4, c1);
    gfx_round_rect(s, in.x + 1, in.y + in.h / 2, in.w - 2, in.h / 2 - 1, 3, c2);
    /* four-pane logo */
    int lx = start_btn.x + 10, ly = start_btn.y + (start_btn.h - 14) / 2;
    gfx_fill_rect(s, lx, ly, 6, 6, RGB(255, 90, 60));
    gfx_fill_rect(s, lx + 8, ly, 6, 6, RGB(120, 220, 80));
    gfx_fill_rect(s, lx, ly + 8, 6, 6, RGB(70, 160, 255));
    gfx_fill_rect(s, lx + 8, ly + 8, 6, 6, RGB(255, 210, 50));
    gfx_ui_text_bold(s, start_btn.x + 32, start_btn.y + (start_btn.h - 16) / 2, "Start", 0xFFFFFF);
}

void taskbar_draw(surface_t *s)
{
    if (!rect_overlaps(bar, s->clip)) return;
    layout();
    gfx_gradient_v(s, bar.x, bar.y, bar.w, bar.h, RGB(44, 50, 64), C_TASKBAR);
    gfx_hline(s, bar.x, bar.y, bar.w, RGB(90, 100, 124));
    draw_start(s);
    window_t *f = wm_focused();
    for (int i = 0; i < nbtn; i++) {
        window_t *w = btn_win[i];
        rect_t r = btn_rect[i];
        bool active = w == f && !w->minimized;
        u32 bg = active ? C_TASKBAR_BTN_ACT : hover == i ? RGB(66, 74, 94) : C_TASKBAR_BTN;
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 3, bg);
        if (active) gfx_hline(s, r.x + 3, r.y + r.h - 2, r.w - 6, RGB(90, 170, 255));
        icon_draw_small(s, w->icon, r.x + 6, r.y + (r.h - 16) / 2);
        gfx_ui_text_ellipsis(s, r.x + 26, r.y + (r.h - 16) / 2, w->title, r.w - 32,
                          w->minimized ? RGB(170, 175, 190) : C_TASKBAR_TEXT);
    }
    /* system tray: disk + memory meter */
    int tx = SCREEN_W - CLOCK_W - TRAY_W;
    gfx_vline(s, tx - 4, bar.y + 6, bar.h - 12, RGB(70, 78, 98));
    icon_draw_small(s, ICON_DRIVE, tx + 4, bar.y + 10);
    gfx_fill_rect(s, tx + 26, bar.y + 9, 8, 18, RGB(20, 22, 28));
    int mh = 16 * mem_used_pct / 100;
    gfx_fill_rect(s, tx + 27, bar.y + 10 + 16 - mh, 6, mh, mem_used_pct > 80 ? RGB(240, 80, 60) : RGB(80, 200, 120));
    /* clock */
    struct rtc_time t;
    clock_now(&t);
    char tbuf[16], dbuf[16];
    if (settings_get_int("clock24", 1)) snprintf(tbuf, sizeof tbuf, "%02d:%02d", t.hour, t.minute);
    else snprintf(tbuf, sizeof tbuf, "%d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour < 12 ? "AM" : "PM");
    snprintf(dbuf, sizeof dbuf, "%04d-%02d-%02d", t.year, t.month, t.day);
    rect_t cr = rect_make(SCREEN_W - CLOCK_W, bar.y, CLOCK_W, bar.h);
    gfx_ui_text(s, cr.x + (cr.w - gfx_ui_width(tbuf)) / 2, bar.y + 2, tbuf, C_TASKBAR_TEXT);
    gfx_ui_text(s, cr.x + (cr.w - gfx_ui_width(dbuf)) / 2, bar.y + 18, dbuf, RGB(170, 178, 196));
}

static void start_cb(int id, void *ctx)
{
    if (id >= SM_APP && id < SM_APP + APP_COUNT) app_launch(id - SM_APP, NULL);
    else if (id == SM_PROPS) sysprops_open();
    else if (id == SM_RESTART) system_restart();
    else if (id == SM_SHUTDOWN) system_shutdown();
}

void taskbar_toggle_start(void)
{
    if (menu_is_banner()) { menu_close(); taskbar_changed(); return; }
    menu_item_t items[MENU_MAX_ITEMS];
    int n = 0;
    for (int a = 0; a < APP_COUNT; a++) {
        if (!app_visible(a)) continue;
        items[n++] = (menu_item_t){ g_apps[a].name, SM_APP + a, 0, g_apps[a].icon };
    }
    items[n++] = (menu_item_t){ "", 0, MENU_SEP, -1 };
    items[n++] = (menu_item_t){ "System Properties", SM_PROPS, 0, ICON_SETTINGS };
    items[n++] = (menu_item_t){ "", 0, MENU_SEP, -1 };
    items[n++] = (menu_item_t){ "Restart", SM_RESTART, 0, ICON_RESTART };
    items[n++] = (menu_item_t){ "Shut Down", SM_SHUTDOWN, 0, ICON_SHUTDOWN };
    menu_open_above(0, bar.y, items, n, start_cb, NULL, true);
    taskbar_changed();
    klog("[ui  ] start menu opened\n");
}

static void win_menu_cb(int id, void *ctx)
{
    window_t *w = ctx;
    if (!wm_window_alive(w)) return;
    if (id == WM_RESTORE) { if (w->maximized) wm_toggle_maximize(w); wm_focus(w); }
    else if (id == WM_MINIMIZE) wm_minimize(w);
    else if (id == WM_MAXIMIZE) { wm_focus(w); if (!w->maximized) wm_toggle_maximize(w); }
    else if (id == WM_CLOSE) wm_close(w);
}

void taskbar_mouse_down(int x, int y, int button)
{
    if (rect_contains(start_btn, x, y)) {
        if (button == MOUSE_LEFT) taskbar_toggle_start();
        return;
    }
    layout();
    for (int i = 0; i < nbtn; i++) {
        if (!rect_contains(btn_rect[i], x, y)) continue;
        window_t *w = btn_win[i];
        if (button == MOUSE_RIGHT) {
            menu_item_t items[] = {
                { "Restore", WM_RESTORE, (w->minimized || w->maximized) ? 0 : MENU_DISABLED, -1 },
                { "Minimize", WM_MINIMIZE, w->minimized ? MENU_DISABLED : 0, -1 },
                { "Maximize", WM_MAXIMIZE, (w->maximized || !(w->flags & WF_RESIZABLE)) ? MENU_DISABLED : 0, -1 },
                { "", 0, MENU_SEP, -1 },
                { "Close", WM_CLOSE, 0, -1 },
            };
            menu_open_above(x, bar.y, items, 5, win_menu_cb, w, false);
            return;
        }
        if (w->minimized) { klog("[ui  ] taskbar: restore \"%s\"\n", w->title); wm_focus(w); }
        else if (w == wm_focused()) { klog("[ui  ] taskbar: minimize \"%s\"\n", w->title); wm_minimize(w); }
        else wm_focus(w);
        return;
    }
}

void taskbar_mouse_move(int x, int y)
{
    int h = -2;
    if (rect_contains(bar, x, y)) {
        if (rect_contains(start_btn, x, y)) h = -1;
        layout();
        for (int i = 0; i < nbtn; i++) if (rect_contains(btn_rect[i], x, y)) h = i;
    }
    if (h != hover) { hover = h; taskbar_changed(); }
}

void taskbar_tick(void)
{
    static u64 last_check;
    u64 now = timer_ticks();
    if (now - last_check < 1000) return;
    last_check = now;
    struct rtc_time t;
    clock_now(&t);
    if (t.minute != last_minute) {
        if (last_minute >= 0) resync_clock();
        last_minute = t.minute;
        wm_damage(rect_make(SCREEN_W - CLOCK_W, bar.y, CLOCK_W, bar.h));
    }
    u32 pct = 100 - pmm_free_frames() * 100 / MAX(1u, pmm_total_frames());
    if (pct != mem_used_pct) {
        mem_used_pct = pct;
        wm_damage(rect_make(SCREEN_W - CLOCK_W - TRAY_W, bar.y, TRAY_W, bar.h));
    }
}

/* ---- shutdown / restart ---- */
static void farewell(const char *msg)
{
    nbfs_sync(ROOTFS);
    surface_t fb;
    surface_wrap(&fb, g_fb.pixels, g_fb.width, g_fb.height, g_fb.pitch);
    gfx_gradient_v(&fb, 0, 0, fb.w, fb.h, RGB(0, 70, 140), RGB(0, 30, 70));
    rect_t r = rect_make(0, fb.h / 2 - 20, fb.w, 40);
    gfx_ui_centered(&fb, r, msg, 0xFFFFFF);
}

void system_shutdown(void)
{
    klog("[ui  ] shut down requested\n");
    farewell("NimbusOS is shutting down...");
    power_shutdown();
}

void system_restart(void)
{
    klog("[ui  ] restart requested\n");
    farewell("NimbusOS is restarting...");
    power_reboot();
}

static void shutdown_cb(int r, void *ctx)
{
    if (r == DLG_YES) system_shutdown();
}

void taskbar_shutdown_prompt(void)
{
    msgbox(NULL, "Shut Down NimbusOS", "Do you want to shut down the computer?", ICON_SHUTDOWN, MB_YESNO, shutdown_cb, NULL);
}
