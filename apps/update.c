/*
 * NimbusOS - NimbusUpdate: checks the update server, shows what a new
 * release contains, downloads and verifies it, installs it and offers to
 * restart.  The work is done by kernel/update.c.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "kernel/update.h"
#include "kernel/version.h"
#include "gui/taskbar.h"
#include "kernel/heap.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "fs/vfs.h"

struct upd_app {
    struct update *u;
    rect_t check_btn, install_btn, restart_btn;
};

static window_t *upd_win;

static void paint(window_t *w)
{
    struct upd_app *a = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    icon_draw(s, ICON_UPDATE, 20, 18);
    gfx_text_scaled(s, 64, 20, "NimbusUpdate", 2, C_TEXT);
    char b[300];
    snprintf(b, sizeof b, "Installed: NimbusOS %s", NIMBUS_VERSION);
    gfx_ui_text(s, 20, 66, b, C_TEXT);
    snprintf(b, sizeof b, "Server: %s", update_server());
    gfx_ui_text_ellipsis(s, 20, 88, b, s->w - 40, C_TEXT_DIM);
    if (vfs_booted_from_ramdisk())
        gfx_ui_text(s, 20, 110, "Running from the live CD: install NimbusOS to a disk to update it.", RGB(180, 90, 20));

    enum update_phase ph = update_phase(a->u);
    u32 col = ph == UPD_FAILED ? RGB(200, 40, 40) : ph == UPD_DONE || ph == UPD_UPTODATE ? RGB(30, 130, 60) : C_TEXT;
    gfx_ui_text_ellipsis(s, 20, 138, update_status(a->u), s->w - 40, col);

    int y = 166;
    if (ph == UPD_DOWNLOADING || ph == UPD_DONE) {
        u32 done, total;
        update_progress(a->u, &done, &total);
        ui_progress(s, rect_make(20, y, s->w - 40, 16), total ? (int)((u64)done * 100 / total) : 100);
        snprintf(b, sizeof b, "%u of %u KiB downloaded and verified", (done + 1023) / 1024, (total + 1023) / 1024);
        gfx_ui_text(s, 20, y + 22, b, C_TEXT_DIM);
        y += 48;
    }
    if (update_new_version(a->u)[0] && (ph == UPD_AVAILABLE || ph == UPD_DOWNLOADING || ph == UPD_DONE)) {
        snprintf(b, sizeof b, "What's new in %s:", update_new_version(a->u));
        gfx_ui_text_bold(s, 20, y, b, C_TEXT);
        y += 22;
        const char *n = update_notes(a->u);
        while (*n && y < s->h - 70) {
            const char *nl = strchr(n, '\n');
            int len = nl ? (int)(nl - n) : (int)strlen(n);
            char line[200];
            snprintf(line, sizeof line, "\x07 %.*s", MIN(len, 190), n);
            gfx_ui_text_ellipsis(s, 28, y, line, s->w - 56, C_TEXT);
            y += 20;
            n += len + (nl ? 1 : 0);
        }
    }

    bool busy = ph == UPD_CHECKING || ph == UPD_DOWNLOADING;
    int by = s->h - 48;
    a->check_btn = rect_make(20, by, 160, 30);
    a->install_btn = rect_make(192, by, 120, 30);
    a->restart_btn = rect_make(s->w - 150, by, 130, 30);
    ui_button(s, a->check_btn, "Check for updates", busy ? BTN_DISABLED : BTN_NORMAL);
    ui_button(s, a->install_btn, "Install", ph == UPD_AVAILABLE ? BTN_DEFAULT : BTN_DISABLED);
    if (ph == UPD_DONE && update_needs_restart(a->u)) ui_button(s, a->restart_btn, "Restart now", BTN_DEFAULT);
    wm_invalidate(w);
}

static void on_say(void *ctx, bool err, const char *msg)
{
    (void)err; (void)msg;
    if (upd_win && wm_window_alive(upd_win)) paint(upd_win);
}

static void on_event(window_t *w, event_t *e)
{
    struct upd_app *a = w->app;
    switch (e->type) {
    case EV_PAINT: case EV_RESIZE: case EV_FOCUS: case EV_BLUR: paint(w); break;
    case EV_TIMER: {
        update_poll(a->u);
        enum update_phase ph = update_phase(a->u);
        if (ph != UPD_CHECKING && ph != UPD_DOWNLOADING) wm_set_timer(w, 0);
        paint(w);
        break;
    }
    case EV_MOUSE_DOWN: {
        enum update_phase ph = update_phase(a->u);
        bool busy = ph == UPD_CHECKING || ph == UPD_DOWNLOADING;
        if (!busy && rect_contains(a->check_btn, e->x, e->y)) { update_check(a->u, false); wm_set_timer(w, 50); }
        else if (ph == UPD_AVAILABLE && rect_contains(a->install_btn, e->x, e->y)) { update_install(a->u); wm_set_timer(w, 50); }
        else if (ph == UPD_DONE && update_needs_restart(a->u) && rect_contains(a->restart_btn, e->x, e->y)) system_restart();
        paint(w);
        break;
    }
    case EV_KEY:
        if (e->key.pressed && e->key.key == KEY_ENTER && update_phase(a->u) == UPD_AVAILABLE) { update_install(a->u); wm_set_timer(w, 50); paint(w); }
        break;
    case EV_CLOSE:
        if (update_phase(a->u) == UPD_DOWNLOADING) break;     /* don't interrupt an install */
        wm_destroy_window(w);
        break;
    case EV_DESTROY:
        update_free(a->u);
        kfree(a);
        if (upd_win == w) upd_win = NULL;
        break;
    }
}

void update_open(const char *unused)
{
    if (upd_win && wm_window_alive(upd_win)) { wm_focus(upd_win); return; }
    struct upd_app *a = kzalloc(sizeof *a);
    if (!a) return;
    a->u = update_new(on_say, NULL);
    window_t *w = wm_create("NimbusUpdate", ICON_UPDATE, 560, 420, 0, on_event, a);
    if (!w || !a->u) { update_free(a->u); kfree(a); return; }
    upd_win = w;
    update_check(a->u, false);                 /* check right away */
    wm_set_timer(w, 50);
    paint(w);
}
