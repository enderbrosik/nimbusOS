/*
 * NimbusOS - Settings.
 *
 * Pages: Display (resolution, applied live with a 15 s "keep?" countdown
 * that reverts on its own), Appearance (accent colour, smooth UI font,
 * wallpaper), Mouse (pointer speed, double-click time), Date & Time
 * (time zone offset, 12/24 hour clock).  Everything is stored with
 * kernel/settings.c in /System/settings.cfg.
 */
#include "apps/apps.h"
#include "gui/dialog.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "gui/desktop.h"
#include "gui/taskbar.h"
#include "drivers/fb.h"
#include "drivers/mouse.h"
#include "kernel/settings.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/sound.h"
#include "drivers/ac97.h"
#include "net/net.h"
#include "net/netjob.h"

#define NAV_W 150
#define ROW 26

enum { PG_DISPLAY, PG_APPEARANCE, PG_MOUSE, PG_SOUND, PG_NETWORK, PG_TIME, PG_CURSOR, NPAGES };
static const char *page_names[NPAGES] = { "Display", "Appearance", "Mouse", "Sound", "Network", "Date & Time", "Cursor" };
static const int page_icons[NPAGES] = { ICON_COMPUTER, ICON_WALLPAPER, ICON_SETTINGS, ICON_AUDIO, ICON_NETWORK, ICON_INFO, ICON_SETTINGS };

/* Resolutions, smallest first. The name is shown as "1920 x 1080  (1080p, 16:9)". */
static const struct { int w, h; const char *name, *ratio; } modes[] = {
    { 320, 180, "180p", "16:9" },      { 426, 240, "240p", "16:9" },      { 640, 360, "360p", "16:9" },
    { 854, 480, "480p", "16:9" },      { 800, 600, "SVGA", "4:3" },       { 1024, 768, "XGA", "4:3" },
    { 1280, 720, "720p", "16:9" },     { 1280, 800, "WXGA", "16:10" },    { 1280, 1024, "SXGA", "5:4" },
    { 1366, 768, "", "16:9" },         { 1440, 900, "WXGA+", "16:10" },   { 1600, 900, "900p", "16:9" },
    { 1680, 1050, "WSXGA+", "16:10" }, { 1920, 1080, "1080p", "16:9" },   { 1920, 1200, "WUXGA", "16:10" },
    { 2560, 1080, "UltraWide", "21:9" }, { 2560, 1440, "1440p", "16:9" }, { 3840, 2160, "4K", "16:9" },
    { 1080, 1080, "Square", "1:1" },   { 1080, 1350, "", "4:5" },         { 1080, 1620, "", "2:3" },
    { 1620, 1080, "", "3:2" },         { 1080, 1920, "Portrait", "9:16" },
};
#define MODE_ROWS 14                    /* visible rows of the resolution list */
static const u32 accents[8] = {
    RGB(0, 120, 215), RGB(16, 124, 16), RGB(194, 57, 52), RGB(135, 100, 184),
    RGB(202, 80, 16), RGB(0, 153, 188), RGB(232, 17, 123), RGB(76, 74, 72),
};
static const char *pointer_names[5] = { "Arrow", "Dot", "Ring", "Plus", "Image file" };
#define CUR_BTN_Y 138                    /* the "Choose image..." button, under the style list */
static void cursor_file_chosen(bool ok, const char *path, void *ctx);
static void paint(window_t *w);
static const u32 pointer_fills[6] = { 0xFFFFFF, 0xFFE27A, 0x7FE8FF, 0x9CFF8A, 0xFF9CC8, 0xFFB347 };
static const int ui_scales[6] = { 75, 100, 125, 150, 175, 200 };
static const char *scale_names[6] = { "75%", "100%", "125%", "150%", "175%", "200%" };
static const int speeds[5] = { 2, 3, 4, 6, 8 };
static const char *speed_names[5] = { "Slow", "Slower", "Normal", "Faster", "Fast" };
static const int dbl_times[3] = { 300, 450, 650 };

struct settings_app {
    int page;
    int sel_mode;
    int mode_scroll;                /* first resolution shown in the list */
    rect_t apply_btn, wall_btn, tz_minus, tz_plus, vol_bar, test_btn, renew_btn;
    struct netjob *job;
    bool drag_vol;
    /* "keep display settings?" countdown */
    window_t *confirm;
    int old_w, old_h, countdown;
};

static window_t *set_win;

static rect_t content(window_t *w) { return rect_make(NAV_W + 20, 16, w->surface->w - NAV_W - 36, w->surface->h - 32); }

static void heading(surface_t *s, rect_t c, const char *t)
{
    gfx_ui_text_bold(s, c.x, c.y, t, C_TEXT);
    gfx_hline(s, c.x, c.y + 22, c.w, RGB(220, 220, 226));
}

static void cursor_file_chosen(bool ok, const char *path, void *ctx)
{
    window_t *w = ctx;
    if (!ok || !wm_window_alive(w)) return;
    if (!wm_set_cursor_file(path)) {
        msgbox(w, "Pointer", "That file is not a BMP picture of 32 x 32 pixels or smaller.", ICON_ERROR, MB_OK, NULL, NULL);
        return;
    }
    settings_set("cursor_file", path);
    settings_set_int("cursor_style", 4);
    wm_set_cursor_look(4, settings_get_int("cursor_scale", 1), (u32)settings_get_int("cursor_fill", 0xFFFFFF));
    paint(w);
}

static void paint(window_t *w)
{
    struct settings_app *a = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFF);
    /* navigation */
    gfx_fill_rect(s, 0, 0, NAV_W, s->h, RGB(243, 245, 249));
    gfx_vline(s, NAV_W - 1, 0, s->h, RGB(222, 225, 232));
    for (int i = 0; i < NPAGES; i++) {
        rect_t r = rect_make(6, 12 + i * 34, NAV_W - 12, 30);
        if (i == a->page) {
            gfx_round_rect(s, r.x, r.y, r.w, r.h, 4, C_SELECT_LIGHT);
            gfx_fill_rect(s, r.x, r.y + 6, 3, r.h - 12, C_SELECT);
        }
        icon_draw_small(s, page_icons[i], r.x + 10, r.y + 7);
        gfx_ui_text(s, r.x + 34, r.y + 7, page_names[i], C_TEXT);
    }
    rect_t c = content(w);
    char b[96];
    switch (a->page) {
    case PG_DISPLAY: {
        heading(s, c, "Display resolution");
        int y = c.y + 34;
        int last = MIN(a->mode_scroll + MODE_ROWS, (int)ARRAY_SIZE(modes));
        for (int i = a->mode_scroll; i < last; i++) {
            bool ok = fb_mode_supported(modes[i].w, modes[i].h);
            bool current = modes[i].w == (int)g_fb.width && modes[i].h == (int)g_fb.height;
            char tag[48];
            if (modes[i].name[0]) snprintf(tag, sizeof tag, "%s, %s", modes[i].name, modes[i].ratio);
            else snprintf(tag, sizeof tag, "%s", modes[i].ratio);
            snprintf(b, sizeof b, "%4d x %-4d  (%s)%s%s", modes[i].w, modes[i].h, tag,
                     current ? "   current" : "", ok ? "" : "   needs more video memory");
            ui_radio(s, c.x, y, i == a->sel_mode, NULL);
            gfx_ui_text(s, c.x + 22, y, b, ok ? C_TEXT : C_TEXT_DIM);
            y += ROW;
        }
        if (ARRAY_SIZE(modes) > MODE_ROWS) {
            snprintf(b, sizeof b, "%d-%d of %d, wheel scrolls", a->mode_scroll + 1, last, (int)ARRAY_SIZE(modes));
            gfx_ui_text(s, c.x + c.w - 170, c.y, b, C_TEXT_DIM);
        }
        snprintf(b, sizeof b, "Video memory: %u MiB", fb_vram_bytes() >> 20);
        gfx_ui_text(s, c.x, y + 6, b, C_TEXT_DIM);
        a->apply_btn = rect_make(c.x + c.w - 100, c.y + c.h - 30, 100, 28);
        ui_button(s, a->apply_btn, "Apply", BTN_DEFAULT);
        break;
    }
    case PG_APPEARANCE: {
        heading(s, c, "Accent colour");
        for (int i = 0; i < 8; i++) {
            rect_t r = rect_make(c.x + i * 44, c.y + 36, 36, 36);
            gfx_round_rect(s, r.x, r.y, r.w, r.h, 6, accents[i]);
            if (accents[i] == g_accent) { gfx_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, C_TEXT); }
        }
        heading(s, rect_make(c.x, c.y + 96, c.w, 0), "Text");
        ui_checkbox(s, c.x, c.y + 130, g_ui_smooth, NULL);
        gfx_ui_text(s, c.x + 20, c.y + 130, "Smooth proportional font for menus, titles and labels", C_TEXT);
        gfx_ui_text(s, c.x + 20, c.y + 152, g_ui_smooth ? "The quick brown fox jumps over the lazy dog." : "", C_TEXT_DIM);
        heading(s, rect_make(c.x, c.y + 186, c.w, 0), "Background");
        a->wall_btn = rect_make(c.x, c.y + 220, 180, 28);
        ui_button(s, a->wall_btn, "Change Wallpaper...", BTN_NORMAL);
        heading(s, rect_make(c.x, c.y + 270, c.w, 0), "UI scale");
        for (int i = 0; i < (int)ARRAY_SIZE(ui_scales); i++) {
            rect_t r = rect_make(c.x + i * 80, c.y + 298, 72, 28);
            ui_button(s, r, scale_names[i], ui_scales[i] == wm_ui_scale() ? BTN_DEFAULT : BTN_NORMAL);
        }
        gfx_ui_text(s, c.x, c.y + 336, "Makes windows, text and icons bigger or smaller.", C_TEXT_DIM);
        break;
    }
    case PG_CURSOR: {
        heading(s, c, "Pointer style");
        int style = settings_get_int("cursor_style", 0);
        for (int i = 0; i < 5; i++) {
            ui_radio(s, c.x, c.y + 34 + i * ROW, style == i, NULL);
            gfx_ui_text(s, c.x + 22, c.y + 34 + i * ROW, pointer_names[i], C_TEXT);
        }
        ui_button(s, rect_make(c.x + 200, c.y + CUR_BTN_Y - 2, 150, 24), "Choose image...", BTN_NORMAL);
        heading(s, rect_make(c.x, c.y + 150, c.w, 0), "Size");
        int scale = settings_get_int("cursor_scale", 1);
        for (int i = 0; i < 3; i++) {
            ui_radio(s, c.x, c.y + 184 + i * ROW, scale == i + 1, NULL);
            char sz[8];
            snprintf(sz, sizeof sz, "%dx", i + 1);
            gfx_ui_text(s, c.x + 22, c.y + 184 + i * ROW, sz, C_TEXT);
        }
        heading(s, rect_make(c.x, c.y + 270, c.w, 0), "Colour");
        u32 fill = (u32)settings_get_int("cursor_fill", 0xFFFFFF);
        for (int i = 0; i < 6; i++) {
            rect_t r = rect_make(c.x + i * 44, c.y + 304, 36, 36);
            gfx_round_rect(s, r.x, r.y, r.w, r.h, 6, pointer_fills[i]);
            gfx_rect(s, r.x, r.y, r.w, r.h, 0x202020);
            if (pointer_fills[i] == fill) gfx_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, C_TEXT);
        }
        gfx_ui_text(s, c.x, c.y + 352, "Pointer changes apply straight away.", C_TEXT_DIM);
        break;
    }
    case PG_MOUSE: {
        heading(s, c, "Pointer speed");
        int cur = settings_get_int("mouse_speed", 4);
        for (int i = 0; i < 5; i++) {
            ui_radio(s, c.x, c.y + 34 + i * ROW, speeds[i] == cur, NULL);
            gfx_ui_text(s, c.x + 22, c.y + 34 + i * ROW, speed_names[i], C_TEXT);
        }
        heading(s, rect_make(c.x, c.y + 176, c.w, 0), "Double-click speed");
        static const char *dn[3] = { "Fast (300 ms)", "Normal (450 ms)", "Slow (650 ms)" };
        for (int i = 0; i < 3; i++) {
            ui_radio(s, c.x, c.y + 210 + i * ROW, dbl_times[i] == g_dblclick_ms, NULL);
            gfx_ui_text(s, c.x + 22, c.y + 210 + i * ROW, dn[i], C_TEXT);
        }
        gfx_ui_text(s, c.x, c.y + 300, "The mouse wheel scrolls lists, documents and the terminal.", C_TEXT_DIM);
        break;
    }
    case PG_SOUND: {
        heading(s, c, "Output device");
        if (sound_available()) snprintf(b, sizeof b, "Intel AC'97 audio, %u Hz stereo", ac97_rate());
        else snprintf(b, sizeof b, "No sound card found (QEMU: -device AC97)");
        gfx_ui_text(s, c.x, c.y + 34, b, sound_available() ? C_TEXT : C_TEXT_DIM);
        heading(s, rect_make(c.x, c.y + 70, c.w, 0), "Volume");
        a->vol_bar = rect_make(c.x, c.y + 108, 260, 10);
        gfx_round_rect(s, a->vol_bar.x, a->vol_bar.y, a->vol_bar.w, a->vol_bar.h, 4, RGB(220, 222, 228));
        int vx = a->vol_bar.x + a->vol_bar.w * sound_volume() / 100;
        gfx_round_rect(s, a->vol_bar.x, a->vol_bar.y, vx - a->vol_bar.x, a->vol_bar.h, 4, C_SELECT);
        gfx_fill_circle(s, vx, a->vol_bar.y + 5, 8, C_SELECT);
        snprintf(b, sizeof b, "%d%%", sound_volume());
        gfx_ui_text(s, a->vol_bar.x + a->vol_bar.w + 16, a->vol_bar.y - 3, b, C_TEXT);
        ui_checkbox(s, c.x, c.y + 136, sound_muted(), NULL);
        gfx_ui_text(s, c.x + 20, c.y + 136, "Mute", C_TEXT);
        heading(s, rect_make(c.x, c.y + 176, c.w, 0), "System sounds");
        ui_checkbox(s, c.x, c.y + 210, settings_get_int("system_sounds", 1), NULL);
        gfx_ui_text(s, c.x + 20, c.y + 210, "Play sounds for startup, dialogs and games", C_TEXT);
        a->test_btn = rect_make(c.x, c.y + 244, 140, 28);
        ui_button(s, a->test_btn, "Play test sound", sound_available() ? BTN_NORMAL : BTN_DISABLED);
        break;
    }
    case PG_NETWORK: {
        heading(s, c, "Ethernet");
        char ipb[16];
        if (!g_net.present) {
            gfx_ui_text(s, c.x, c.y + 34, "No network card found (QEMU: -nic user,model=e1000)", C_TEXT_DIM);
            break;
        }
        snprintf(b, sizeof b, "Intel e1000, MAC %02x:%02x:%02x:%02x:%02x:%02x", g_net.mac[0], g_net.mac[1], g_net.mac[2],
                 g_net.mac[3], g_net.mac[4], g_net.mac[5]);
        gfx_ui_text(s, c.x, c.y + 34, b, C_TEXT);
        static const char *labels[4] = { "IP address", "Subnet mask", "Gateway", "DNS server" };
        u32 vals[4] = { g_net.ip, g_net.mask, g_net.gw, g_net.dns };
        for (int i = 0; i < 4; i++) {
            gfx_ui_text(s, c.x, c.y + 66 + i * 24, labels[i], C_TEXT_DIM);
            if (g_net.configured) ip_to_str(vals[i], ipb); else strcpy(ipb, "-");
            gfx_ui_text(s, c.x + 130, c.y + 66 + i * 24, ipb, C_TEXT);
        }
        snprintf(b, sizeof b, "Received %u packets, sent %u packets", g_net.rx_packets, g_net.tx_packets);
        gfx_ui_text(s, c.x, c.y + 172, b, C_TEXT_DIM);
        gfx_ui_text(s, c.x, c.y + 194, a->job ? "Requesting an address..." : g_net.configured ? "Connected (DHCP)" : "Not connected",
                    g_net.configured ? RGB(30, 130, 60) : C_TEXT_DIM);
        a->renew_btn = rect_make(c.x, c.y + 226, 150, 28);
        ui_button(s, a->renew_btn, "Renew DHCP lease", a->job ? BTN_DISABLED : BTN_NORMAL);
        break;
    }
    case PG_TIME: {
        heading(s, c, "Date and time");
        struct rtc_time t;
        clock_now(&t);
        snprintf(b, sizeof b, "%02d:%02d:%02d", t.hour, t.minute, t.second);
        gfx_text_scaled(s, c.x, c.y + 36, b, 3, C_TEXT);
        snprintf(b, sizeof b, "%04d-%02d-%02d", t.year, t.month, t.day);
        gfx_ui_text(s, c.x + 4, c.y + 92, b, C_TEXT_DIM);
        heading(s, rect_make(c.x, c.y + 124, c.w, 0), "Time zone");
        int tz = settings_get_int("tz_minutes", 0);
        snprintf(b, sizeof b, "UTC%c%02d:%02d", tz < 0 ? '-' : '+', ABS(tz) / 60, ABS(tz) % 60);
        a->tz_minus = rect_make(c.x, c.y + 156, 30, 26);
        a->tz_plus = rect_make(c.x + 150, c.y + 156, 30, 26);
        ui_button(s, a->tz_minus, "-", BTN_NORMAL);
        ui_button(s, a->tz_plus, "+", BTN_NORMAL);
        gfx_ui_centered(s, rect_make(c.x + 30, c.y + 156, 120, 26), b, C_TEXT);
        gfx_ui_text(s, c.x, c.y + 190, "The hardware clock keeps UTC; the offset is applied for display.", C_TEXT_DIM);
        heading(s, rect_make(c.x, c.y + 224, c.w, 0), "Clock format");
        ui_checkbox(s, c.x, c.y + 258, settings_get_int("clock24", 1), NULL);
        gfx_ui_text(s, c.x + 20, c.y + 258, "Use 24-hour time", C_TEXT);
        break;
    }
    }
    wm_invalidate(w);
}

/* ---- display confirmation ---- */
static void confirm_close(struct settings_app *a, bool keep)
{
    window_t *cw = a->confirm;
    a->confirm = NULL;
    if (wm_window_alive(cw)) wm_destroy_window(cw);
    if (keep) {
        char b[24];
        snprintf(b, sizeof b, "%ux%u", g_fb.width, g_fb.height);
        settings_set("resolution", b);
        klog("[sett] kept resolution %s\n", b);
    } else {
        wm_set_resolution(a->old_w, a->old_h);
        klog("[sett] reverted to %dx%d\n", a->old_w, a->old_h);
    }
    if (wm_window_alive(set_win)) paint(set_win);
}

static rect_t keep_btn, revert_btn;

static void confirm_paint(window_t *cw)
{
    struct settings_app *a = set_win->app;
    surface_t *s = cw->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFF);
    icon_draw(s, ICON_COMPUTER, 16, 16);
    gfx_ui_text_bold(s, 60, 16, "Keep these display settings?", C_TEXT);
    char b[64];
    snprintf(b, sizeof b, "Reverting to %dx%d in %d seconds.", a->old_w, a->old_h, a->countdown);
    gfx_ui_text(s, 60, 40, b, C_TEXT_DIM);
    keep_btn = rect_make(s->w - 220, s->h - 40, 100, 28);
    revert_btn = rect_make(s->w - 112, s->h - 40, 100, 28);
    ui_button(s, keep_btn, "Keep changes", BTN_DEFAULT);
    ui_button(s, revert_btn, "Revert", BTN_NORMAL);
    wm_invalidate(cw);
}

static void confirm_event(window_t *cw, event_t *e)
{
    if (!wm_window_alive(set_win)) { if (e->type != EV_DESTROY) wm_destroy_window(cw); return; }
    struct settings_app *a = set_win->app;
    switch (e->type) {
    case EV_PAINT: confirm_paint(cw); break;
    case EV_TIMER:
        if (--a->countdown <= 0) confirm_close(a, false);
        else confirm_paint(cw);
        break;
    case EV_MOUSE_UP:
        if (rect_contains(keep_btn, e->x, e->y)) confirm_close(a, true);
        else if (rect_contains(revert_btn, e->x, e->y)) confirm_close(a, false);
        break;
    case EV_KEY:
        if (e->key.pressed && e->key.key == KEY_ENTER) confirm_close(a, true);
        else if (e->key.pressed && e->key.key == KEY_ESC) confirm_close(a, false);
        break;
    case EV_CLOSE: confirm_close(a, false); break;
    case EV_DESTROY: if (a->confirm == cw) a->confirm = NULL; break;
    }
}

static void apply_mode(window_t *w)
{
    struct settings_app *a = w->app;
    if (a->sel_mode < 0) return;
    int nw = modes[a->sel_mode].w, nh = modes[a->sel_mode].h;
    if (nw == (int)g_fb.width && nh == (int)g_fb.height) return;
    a->old_w = g_fb.width;
    a->old_h = g_fb.height;
    if (!wm_set_resolution(nw, nh)) {
        klog("[sett] mode %dx%d failed\n", nw, nh);
        return;
    }
    a->countdown = 15;
    a->confirm = wm_create("Display Settings", ICON_COMPUTER, 380, 120, WF_DIALOG | WF_NO_TASKBAR, confirm_event, NULL);
    if (a->confirm) {
        wm_center(a->confirm);
        wm_set_modal(w, a->confirm);
        wm_set_timer(a->confirm, 1000);
    }
    paint(w);
}

static void on_event(window_t *w, event_t *e)
{
    if (e->type == EV_WHEEL && ((struct settings_app *)w->app)->page == PG_DISPLAY) {
        struct settings_app *a = w->app;
        int max = MAX(0, (int)ARRAY_SIZE(modes) - MODE_ROWS);
        a->mode_scroll = MAX(0, MIN(max, a->mode_scroll + e->wheel));
        paint(w);
        return;
    }
    struct settings_app *a = w->app;
    rect_t c = content(w);
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER:
        if (a->job) {
            char line[200];
            bool er;
            while (netjob_next_line(a->job, line, sizeof line, &er)) {}
            if (netjob_done(a->job)) { netjob_release(a->job); a->job = NULL; }
        }
        if (a->page == PG_TIME || a->page == PG_NETWORK) paint(w);
        break;
    case EV_MOUSE_DOWN: {
        if (e->x < NAV_W) {
            int i = (e->y - 12) / 34;
            if (i >= 0 && i < NPAGES) { a->page = i; klog("[sett] page %s\n", page_names[i]); }
            paint(w);
            break;
        }
        switch (a->page) {
        case PG_DISPLAY: {
            int i = a->mode_scroll + (e->y - c.y - 34) / ROW;
            if (e->y >= c.y + 34 && (e->y - c.y - 34) / ROW < MODE_ROWS && i >= 0 && i < (int)ARRAY_SIZE(modes) &&
                e->x < c.x + 360 && fb_mode_supported(modes[i].w, modes[i].h)) a->sel_mode = i;
            if (rect_contains(a->apply_btn, e->x, e->y)) { apply_mode(w); return; }
            break;
        }
        case PG_CURSOR: {
            int style = settings_get_int("cursor_style", 0), scale = settings_get_int("cursor_scale", 1);
            u32 fill = (u32)settings_get_int("cursor_fill", 0xFFFFFF);
            if (rect_contains(rect_make(c.x + 200, c.y + CUR_BTN_Y - 2, 150, 24), e->x, e->y)) {
                filedialog(w, FD_OPEN, "Choose Pointer Image", "/Pictures", "", ".bmp", cursor_file_chosen, w);
                return;
            }
            for (int i = 0; i < 5; i++)
                if (rect_contains(rect_make(c.x, c.y + 34 + i * ROW, 200, 22), e->x, e->y)) {
                    style = i;
                    if (i == 4 && !settings_get("cursor_file", NULL)) {
                        filedialog(w, FD_OPEN, "Choose Pointer Image", "/Pictures", "", ".bmp", cursor_file_chosen, w);
                        return;
                    }
                }
            for (int i = 0; i < 3; i++)
                if (rect_contains(rect_make(c.x, c.y + 184 + i * ROW, 200, 22), e->x, e->y)) scale = i + 1;
            for (int i = 0; i < 6; i++)
                if (rect_contains(rect_make(c.x + i * 44, c.y + 304, 36, 36), e->x, e->y)) fill = pointer_fills[i];
            settings_set_int("cursor_style", style);
            settings_set_int("cursor_scale", scale);
            settings_set_int("cursor_fill", (int)fill);
            wm_set_cursor_look(style, scale, fill);
            paint(w);
            return;
        }
        case PG_APPEARANCE:
            for (int i = 0; i < (int)ARRAY_SIZE(ui_scales); i++)
                if (rect_contains(rect_make(c.x + i * 80, c.y + 298, 72, 28), e->x, e->y)) {
                    wm_set_ui_scale(ui_scales[i]);
                    settings_set_int("ui_scale", ui_scales[i]);
                    paint(w);
                    return;
                }
            for (int i = 0; i < 8; i++)
                if (rect_contains(rect_make(c.x + i * 44, c.y + 36, 36, 36), e->x, e->y)) {
                    theme_set_accent(accents[i]);
                    settings_set_int("accent", (int)accents[i]);
                    wm_repaint_all();
                    return;
                }
            if (rect_contains(rect_make(c.x, c.y + 128, c.w, 20), e->x, e->y)) {
                g_ui_smooth = !g_ui_smooth;
                settings_set_int("smooth_fonts", g_ui_smooth);
                wm_repaint_all();
                return;
            }
            if (rect_contains(a->wall_btn, e->x, e->y)) { wallpaper_chooser_open(); return; }
            break;
        case PG_MOUSE: {
            int i = (e->y - c.y - 34) / ROW;
            if (e->y >= c.y + 34 && i >= 0 && i < 5) {
                settings_set_int("mouse_speed", speeds[i]);
                mouse_set_speed(speeds[i]);
            }
            i = (e->y - c.y - 210) / ROW;
            if (e->y >= c.y + 210 && i >= 0 && i < 3) {
                g_dblclick_ms = dbl_times[i];
                settings_set_int("dblclick_ms", g_dblclick_ms);
            }
            break;
        }
        case PG_SOUND:
            if (rect_contains(rect_make(a->vol_bar.x - 8, a->vol_bar.y - 8, a->vol_bar.w + 16, a->vol_bar.h + 16), e->x, e->y)) {
                a->drag_vol = true;
                sound_set_volume((e->x - a->vol_bar.x) * 100 / a->vol_bar.w);
            }
            if (rect_contains(rect_make(c.x, c.y + 134, 120, 20), e->x, e->y)) {
                sound_set_muted(!sound_muted());
                settings_set_int("muted", sound_muted());
                taskbar_changed();
            }
            if (rect_contains(rect_make(c.x, c.y + 208, 360, 20), e->x, e->y))
                settings_set_int("system_sounds", !settings_get_int("system_sounds", 1));
            if (rect_contains(a->test_btn, e->x, e->y)) {
                int was = settings_get_int("system_sounds", 1);
                if (!was) settings_set_int("system_sounds", 1);
                sound_event(SND_STARTUP);
                if (!was) settings_set_int("system_sounds", 0);
            }
            break;
        case PG_NETWORK:
            if (!a->job && g_net.present && rect_contains(a->renew_btn, e->x, e->y)) {
                char *argv[1] = { "dhcp" };
                a->job = netjob_start(1, argv);
            }
            break;
        case PG_TIME: {
            int tz = settings_get_int("tz_minutes", 0);
            if (rect_contains(a->tz_minus, e->x, e->y) && tz > -720) settings_set_int("tz_minutes", tz - 30);
            if (rect_contains(a->tz_plus, e->x, e->y) && tz < 840) settings_set_int("tz_minutes", tz + 30);
            if (rect_contains(rect_make(c.x, c.y + 256, 200, 20), e->x, e->y))
                settings_set_int("clock24", !settings_get_int("clock24", 1));
            taskbar_changed();
            break;
        }
        }
        paint(w);
        break;
    }
    case EV_MOUSE_MOVE:
        if (a->drag_vol) { sound_set_volume((e->x - a->vol_bar.x) * 100 / a->vol_bar.w); paint(w); }
        break;
    case EV_MOUSE_UP:
        if (a->drag_vol) { a->drag_vol = false; settings_set_int("volume", sound_volume()); }
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: if (a->job) netjob_release(a->job); kfree(a); set_win = NULL; break;
    }
}

void settings_open(const char *page)
{
    if (wm_window_alive(set_win)) { wm_focus(set_win); return; }
    struct settings_app *a = kzalloc(sizeof *a);
    if (!a) return;
    a->sel_mode = -1;
    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++)
        if (modes[i].w == (int)g_fb.width && modes[i].h == (int)g_fb.height) a->sel_mode = i;
    for (int i = 0; page && i < NPAGES; i++) if (!strcasecmp(page, page_names[i])) a->page = i;
    set_win = wm_create("Settings", ICON_SETTINGS, 640, 440, WF_NO_MAXIMIZE, on_event, a);
    if (!set_win) { kfree(a); return; }
    wm_center(set_win);
    wm_set_timer(set_win, 1000);
}
