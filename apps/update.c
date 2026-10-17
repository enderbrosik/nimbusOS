/*
 * NimbusOS - Nimbus Update: checks the update server, shows what a new
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
    rect_t check_btn, install_btn, restart_btn, box;
    int scroll, nlines, vis;
    bool dragging;
};

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
static window_t *upd_win;

/* Cuts the next display line off a note: as many words as fit in max_w pixels.
 * Returns the characters used from n[0..len). */
static int wrap_line(const char *n, int len, int max_w, char *out, int outmax)
{
    int used = 0, last_sp = -1;
    for (int i = 0; i < len && i < outmax - 1; i++) {
        out[i] = n[i]; out[i + 1] = 0;
        if (n[i] == ' ') last_sp = i;
        if (gfx_ui_face_width(out, 0) > max_w) {
            int cut = last_sp > 0 ? last_sp : (i > 0 ? i : 1);
            out[cut] = 0;
            used = cut;
            while (used < len && n[used] == ' ') used++;
            return used;
        }
    }
    memcpy(out, n, MIN(len, outmax - 1));
    out[MIN(len, outmax - 1)] = 0;
    return len;
}

static void paint(window_t *w)
{
    struct upd_app *a = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    icon_draw(s, ICON_UPDATE, 20, 18);
    gfx_text_scaled(s, 64, 20, "Nimbus Update", 2, C_TEXT);
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
        /* the notes sit in a scrollable box, like the terms box of an installer */
        a->box = rect_make(20, y, s->w - 40, s->h - 70 - y);
        a->vis = MAX(1, (a->box.h - 8) / 20);
        int tw = a->box.w - 44, total = 0, ly = a->box.y + 6;
        rect_t old = s->clip;
        gfx_set_clip(s, rect_make(a->box.x + 2, a->box.y + 2, a->box.w - 20, a->box.h - 4));
        /* two passes over the same wrapping: count the display lines, then draw those in view */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                a->nlines = total;
                a->scroll = CLAMP(a->scroll, 0, MAX(0, total - a->vis));
                s->clip = old;
                ui_sunken(s, a->box, 0xFFFFFF);
                gfx_set_clip(s, rect_make(a->box.x + 2, a->box.y + 2, a->box.w - 20, a->box.h - 4));
                total = 0;
            }
            for (const char *n = update_notes(a->u); *n; ) {
                const char *nl = strchr(n, '\n');
                int len = nl ? (int)(nl - n) : (int)strlen(n);
                const char *p = n;
                bool first = true;
                do {
                    char line[200];
                    int used = wrap_line(p, len - (int)(p - n), tw, line, sizeof line);
                    if (pass == 1 && total >= a->scroll && ly + 20 <= a->box.y + a->box.h) {
                        if (first) gfx_fill_rect(s, a->box.x + 10, ly + 7, 5, 5, C_TEXT);
                        gfx_ui_text(s, a->box.x + 22, ly, line, C_TEXT);
                        ly += 20;
                    }
                    total++;
                    first = false;
                    p += used;
                } while (p < n + len);
                n += len + (nl ? 1 : 0);
            }
        }
        s->clip = old;
        ui_vscroll(s, rect_make(a->box.x + a->box.w - 17, a->box.y + 1, 16, a->box.h - 2), a->nlines, a->vis, a->scroll);
    } else {
        a->box = rect_make(0, 0, 0, 0);
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
    case EV_WHEEL: a->scroll = CLAMP(a->scroll - e->wheel * 3, 0, MAX(0, a->nlines - a->vis)); paint(w); break;
    case EV_MOUSE_UP: a->dragging = false; break;
    case EV_MOUSE_MOVE:
        if (a->dragging) {
            a->scroll = ui_vscroll_offset_for(rect_make(0, a->box.y, 16, a->box.h), a->nlines, a->vis, e->y);
            paint(w);
        }
        break;
    case EV_MOUSE_DOWN: {
        if (a->box.w && rect_contains(rect_make(a->box.x + a->box.w - 17, a->box.y, 17, a->box.h), e->x, e->y)) {
            a->dragging = true;
            a->scroll = ui_vscroll_offset_for(rect_make(0, a->box.y, 16, a->box.h), a->nlines, a->vis, e->y);
            paint(w);
            break;
        }
        enum update_phase ph = update_phase(a->u);
        bool busy = ph == UPD_CHECKING || ph == UPD_DOWNLOADING;
        if (!busy && rect_contains(a->check_btn, e->x, e->y)) { update_check(a->u, false); wm_set_timer(w, 50); }
        else if (ph == UPD_AVAILABLE && rect_contains(a->install_btn, e->x, e->y)) { update_install(a->u); wm_set_timer(w, 50); }
        else if (ph == UPD_DONE && update_needs_restart(a->u) && rect_contains(a->restart_btn, e->x, e->y)) system_restart();
        paint(w);
        break;
    }
    case EV_KEY:
        if (e->key.pressed && (e->key.key == KEY_UP || e->key.key == KEY_DOWN || e->key.key == KEY_PGUP || e->key.key == KEY_PGDN || e->key.key == KEY_HOME || e->key.key == KEY_END)) {
            int k = e->key.key, d = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -a->vis : k == KEY_PGDN ? a->vis : k == KEY_HOME ? -9999 : 9999;
            a->scroll = CLAMP(a->scroll + d, 0, MAX(0, a->nlines - a->vis));
            paint(w);
            break;
        }
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
    window_t *w = wm_create("Nimbus Update", ICON_UPDATE, 560, 420, 0, on_event, a);
    if (!w || !a->u) { update_free(a->u); kfree(a); return; }
    upd_win = w;
    update_check(a->u, false);                 /* check right away */
    wm_set_timer(w, 50);
    paint(w);
}
