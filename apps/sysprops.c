/* NimbusOS - System Properties window (desktop "Properties"). */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "fs/vfs.h"
#include "drivers/fb.h"
#include "drivers/ata.h"
#include "drivers/pit.h"
#include "kernel/pmm.h"
#include "kernel/heap.h"
#include "kernel/task.h"
#include "kernel/power.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

static window_t *props_win;
static rect_t ok_btn;
static int ok_state;

static void line(surface_t *s, int y, const char *k, const char *v)
{
    gfx_text(s, 24, y, k, C_TEXT_DIM, COLOR_NONE);
    gfx_text(s, 170, y, v, C_TEXT, COLOR_NONE);
}

static void paint(window_t *w)
{
    surface_t *s = w->surface;
    char b[96];
    gfx_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFF);
    gfx_gradient_v(s, 0, 0, s->w, 70, RGB(0, 84, 166), RGB(0, 120, 215));
    icon_draw(s, ICON_COMPUTER, 20, 18);
    gfx_text_bold(s, 66, 18, "NimbusOS 1.0", 0xFFFFFF);
    gfx_text(s, 66, 38, "A hobby operating system, written from scratch", RGB(220, 235, 255), COLOR_NONE);
    int y = 86;
    line(s, y, "Processor", "x86, 32-bit protected mode"); y += 22;
    snprintf(b, sizeof b, "%u MiB (%u MiB free)", pmm_total_frames() / 256, pmm_free_frames() / 256);
    line(s, y, "Memory", b); y += 22;
    struct heap_stats hs;
    heap_get_stats(&hs);
    snprintf(b, sizeof b, "%u KiB mapped, %u KiB in use", hs.mapped / 1024, hs.used / 1024);
    line(s, y, "Kernel heap", b); y += 22;
    snprintf(b, sizeof b, "%dx%d, 32-bit colour", g_fb.width, g_fb.height);
    line(s, y, "Display", b); y += 22;
    u32 total, freeb;
    nbfs_statfs(ROOTFS, &total, &freeb);
    snprintf(b, sizeof b, "NimbusFS \"%s\", %u of %u MiB free", ROOTFS->sb.label, freeb >> 20, total >> 20);
    line(s, y, "System volume", b); y += 22;
    snprintf(b, sizeof b, "%s (%s)", g_ata[g_root->drive].name, g_ata[g_root->drive].model);
    line(s, y, "Boot disk", b); y += 22;
    u32 up = (u32)(timer_ticks() / 1000);
    snprintf(b, sizeof b, "%u h %02u m %02u s", up / 3600, (up / 60) % 60, up % 60);
    line(s, y, "Uptime", b); y += 22;
    struct task_info ti[16];
    snprintf(b, sizeof b, "%d kernel tasks, %d windows", task_list(ti, 16), wm_window_count());
    line(s, y, "Running", b); y += 22;
    line(s, y, "Power", power_acpi_available() ? "ACPI (S5 soft-off)" : "legacy"); y += 22;
    line(s, y, "Mode", system_is_live() ? "Live system (not installed)" : "Installed"); y += 22;
    ok_btn = rect_make(s->w - 100, s->h - 40, 84, 28);
    ui_button(s, ok_btn, "OK", ok_state ? BTN_PRESSED : BTN_DEFAULT);
    wm_invalidate(w);
}

static void ev(window_t *w, event_t *e)
{
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: paint(w); break;
    case EV_MOUSE_DOWN: if (rect_contains(ok_btn, e->x, e->y)) { ok_state = 1; paint(w); } break;
    case EV_MOUSE_UP:
        if (ok_state && rect_contains(ok_btn, e->x, e->y)) { wm_destroy_window(w); return; }
        ok_state = 0; paint(w);
        break;
    case EV_KEY: if (e->key.pressed && (e->key.key == KEY_ENTER || e->key.key == KEY_ESC)) wm_destroy_window(w); break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: props_win = NULL; break;
    }
}

void sysprops_open(void)
{
    if (wm_window_alive(props_win)) { wm_focus(props_win); return; }
    ok_state = 0;
    props_win = wm_create("System Properties", ICON_SETTINGS, 520, 400, WF_DIALOG, ev, NULL);
    if (props_win) { wm_center(props_win); wm_set_timer(props_win, 1000); }
}
