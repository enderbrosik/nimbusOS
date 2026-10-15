/* Stage 5 placeholders: real applications arrive in Stage 6. */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

static void ph_event(window_t *w, event_t *e)
{
    if (e->type == EV_PAINT) {
        surface_t *s = w->surface;
        gfx_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFF);
        icon_draw(s, w->icon, 20, 20);
        gfx_text(s, 64, 22, w->title, C_TEXT, COLOR_NONE);
        gfx_text(s, 64, 42, (const char *)w->app ? (const char *)w->app : "", C_TEXT_DIM, COLOR_NONE);
        gfx_text(s, 20, 80, "This application is part of Stage 6.", C_TEXT_DIM, COLOR_NONE);
        wm_invalidate(w);
    } else if (e->type == EV_CLOSE) {
        wm_destroy_window(w);
    }
}

static char arg_copy[8][128];
static int arg_n;

static void ph(const char *title, int icon, const char *arg)
{
    char *a = arg_copy[arg_n++ % 8];
    strlcpy(a, arg ? arg : "", 128);
    wm_create(title, icon, 420, 200, WF_RESIZABLE, ph_event, a);
}

void explorer_open(const char *p) { ph("File Explorer", ICON_EXPLORER, p); }
void notepad_open(const char *p) { ph("Notepad", ICON_NOTEPAD, p); }
void paint_open(const char *p) { ph("Paint", ICON_PAINT, p); }
void terminal_open(const char *p) { ph("Terminal", ICON_TERMINAL, p); }
void taskmgr_open(const char *p) { ph("Task Manager", ICON_TASKMGR, p); }
void installer_open(const char *p) { ph("Install NimbusOS", ICON_INSTALLER, p); }
