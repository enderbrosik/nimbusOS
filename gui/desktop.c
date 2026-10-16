/*
 * NimbusOS - the desktop: wallpaper, icon grid, context menus.
 *
 * Icons: one per application plus one per entry of /Desktop.  Positions
 * snap to a grid and are saved in /System/icons.cfg ("key x y" lines).
 * Wallpaper settings live in /System/desktop.cfg:
 *     wallpaper=/System/wallpaper.bmp      (or "none")
 *     color=006384                         (fallback / solid colour)
 */
#include "gui/desktop.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gui/icons.h"
#include "gui/menu.h"
#include "gui/bmp.h"
#include "gui/dialog.h"
#include "gui/widgets.h"
#include "apps/apps.h"
#include "fs/vfs.h"
#include "drivers/pit.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define GRID_W 84
#define GRID_H 86
#define ORIGIN_X 6
#define ORIGIN_Y 6
#define MAX_ICONS 96
#define ROWS ((SCREEN_H - TASKBAR_H - ORIGIN_Y) / GRID_H)
#define COLS ((SCREEN_W - ORIGIN_X) / GRID_W)
#define DESKTOP_DIR "/Desktop"
#define ICONS_CFG "/System/icons.cfg"
#define DESKTOP_CFG "/System/desktop.cfg"

struct dicon {
    char key[80];               /* "app:<n>" or "file:<name>" */
    char label[NBFS_NAME_MAX + 1];
    char path[NBFS_PATH_MAX];
    int icon, app;              /* app = -1 for files */
    bool is_dir, selected;
    int x, y;                   /* top-left of the grid cell */
};

static struct dicon icons[MAX_ICONS];
static int nicons;
static surface_t *wall;                 /* scaled wallpaper, NULL = solid colour */
static u32 wall_color = C_DESKTOP_DEFAULT;
static char wall_path[NBFS_PATH_MAX] = "/System/wallpaper.bmp";
static u32 dir_signature;

/* dragging */
static int press_icon = -1;
static bool dragging;
static int drag_off_x, drag_off_y, drag_x, drag_y, press_x, press_y;

/* ------------------------------------------------------------------ */
static void damage_icon(struct dicon *d) { wm_damage(rect_make(d->x, d->y, GRID_W, GRID_H)); }

/* ---- wallpaper ---------------------------------------------------- */
static void load_wallpaper(void)
{
    if (wall) { surface_destroy(wall); wall = NULL; }
    if (wall_path[0] && strcmp(wall_path, "none")) {
        surface_t *img = bmp_load(wall_path);
        if (img) {
            wall = surface_create(SCREEN_W, SCREEN_H);
            if (wall) {
                /* "fill": scale to cover the screen keeping the aspect ratio, crop the centre */
                int dw = SCREEN_W, dh = img->h * SCREEN_W / img->w;
                if (dh < SCREEN_H) { dh = SCREEN_H; dw = img->w * SCREEN_H / img->h; }
                gfx_blit_scaled(wall, rect_make((SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh), img);
            }
            surface_destroy(img);
            klog("[desk] wallpaper %s loaded\n", wall_path);
        } else {
            klog("[desk] wallpaper %s missing or invalid, using colour %06x\n", wall_path, wall_color);
        }
    }
    wm_damage(rect_make(0, 0, SCREEN_W, SCREEN_H));
}

static void save_desktop_cfg(void)
{
    char buf[400];
    int n = snprintf(buf, sizeof buf, "wallpaper=%s\ncolor=%06x\n", wall_path[0] ? wall_path : "none", wall_color);
    vfs_write_all(DESKTOP_CFG, buf, n);
}

static void load_desktop_cfg(void)
{
    char *cfg;
    if (vfs_read_all(DESKTOP_CFG, &cfg, NULL) != 0) return;
    for (char *line = cfg; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "wallpaper=", 10)) strlcpy(wall_path, line + 10, sizeof wall_path);
        else if (!strncmp(line, "color=", 6)) wall_color = (u32)strtol(line + 6, NULL, 16) & 0xFFFFFF;
        line = nl ? nl + 1 : NULL;
    }
    kfree(cfg);
}

void desktop_set_wallpaper(const char *path, u32 color)
{
    strlcpy(wall_path, path ? path : "none", sizeof wall_path);
    wall_color = color;
    load_wallpaper();
    save_desktop_cfg();
}

/* ---- icon positions ----------------------------------------------- */
static void save_positions(void)
{
    char *buf = kmalloc(nicons * 100 + 1);
    int n = 0;
    for (int i = 0; i < nicons; i++) n += snprintf(buf + n, 100, "%s %d %d\n", icons[i].key, icons[i].x, icons[i].y);
    vfs_write_all(ICONS_CFG, buf, n);
    kfree(buf);
}

static bool cell_taken(int x, int y, int except)
{
    for (int i = 0; i < nicons; i++)
        if (i != except && icons[i].x == x && icons[i].y == y) return true;
    return false;
}

static void snap(int *x, int *y)
{
    int c = (*x - ORIGIN_X + GRID_W / 2) / GRID_W, r = (*y - ORIGIN_Y + GRID_H / 2) / GRID_H;
    c = MAX(0, MIN(COLS - 1, c));
    r = MAX(0, MIN(ROWS - 1, r));
    *x = ORIGIN_X + c * GRID_W;
    *y = ORIGIN_Y + r * GRID_H;
}

static void free_cell(int *x, int *y, int except)
{
    if (!cell_taken(*x, *y, except)) return;
    /* nearest free cell by grid distance */
    int bc = 0, br = 0, best = 1 << 30;
    int c0 = (*x - ORIGIN_X) / GRID_W, r0 = (*y - ORIGIN_Y) / GRID_H;
    for (int c = 0; c < COLS; c++)
        for (int r = 0; r < ROWS; r++) {
            if (cell_taken(ORIGIN_X + c * GRID_W, ORIGIN_Y + r * GRID_H, except)) continue;
            int d = (c - c0) * (c - c0) + (r - r0) * (r - r0);
            if (d < best) { best = d; bc = c; br = r; }
        }
    *x = ORIGIN_X + bc * GRID_W;
    *y = ORIGIN_Y + br * GRID_H;
}

/* Positions of icons from before a refresh, so existing icons stay put. */
static struct { char key[80]; int x, y; } prev_pos[MAX_ICONS];
static int nprev;
static bool use_prev = true;     /* false while re-laying out for a new screen size */

static void apply_saved_positions(void)
{
    char *cfg;
    for (int i = 0; i < nicons; i++) icons[i].x = icons[i].y = -1;
    if (vfs_read_all(ICONS_CFG, &cfg, NULL) == 0) {
        for (char *line = cfg; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            /* key may contain spaces: the last two fields are x and y */
            char *sy = strrchr(line, ' ');
            if (sy) {
                *sy = 0;
                char *sx = strrchr(line, ' ');
                if (sx) {
                    *sx = 0;
                    for (int i = 0; i < nicons; i++)
                        if (!strcmp(icons[i].key, line)) { icons[i].x = atoi(sx + 1); icons[i].y = atoi(sy + 1); }
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        kfree(cfg);
    }
    for (int i = 0; i < nicons && use_prev; i++)
        for (int k = 0; k < nprev; k++)
            if (!strcmp(icons[i].key, prev_pos[k].key)) { icons[i].x = prev_pos[k].x; icons[i].y = prev_pos[k].y; }
    /* validate saved positions, then auto-place the rest column by column */
    for (int i = 0; i < nicons; i++) {
        if (icons[i].x < 0) continue;
        int x = icons[i].x, y = icons[i].y;
        snap(&x, &y);
        icons[i].x = icons[i].y = -1;
        free_cell(&x, &y, i);
        icons[i].x = x; icons[i].y = y;
    }
    /* auto-place the rest in the first free cell, column by column */
    for (int i = 0; i < nicons; i++) {
        if (icons[i].x >= 0) continue;
        icons[i].x = -1000;
        for (int cell = 0; cell < COLS * ROWS; cell++) {
            int x = ORIGIN_X + (cell / ROWS) * GRID_W, y = ORIGIN_Y + (cell % ROWS) * GRID_H;
            if (!cell_taken(x, y, i)) { icons[i].x = x; icons[i].y = y; break; }
        }
    }
}

static u32 compute_signature(void)
{
    struct vfs_item *items;
    int n = vfs_list(DESKTOP_DIR, &items);
    u32 h = 2166136261u;
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        for (const char *p = items[i].name; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        h = (h ^ items[i].type) * 16777619u;
    }
    kfree(items);
    return h ^ (u32)n ^ (system_is_live() ? 0x55 : 0);
}

void desktop_refresh(void)
{
    char selected_key[80] = "";
    for (int i = 0; i < nicons; i++) if (icons[i].selected) strlcpy(selected_key, icons[i].key, sizeof selected_key);
    nprev = nicons;
    for (int i = 0; i < nicons; i++) {
        strlcpy(prev_pos[i].key, icons[i].key, sizeof prev_pos[i].key);
        prev_pos[i].x = icons[i].x;
        prev_pos[i].y = icons[i].y;
    }
    nicons = 0;
    for (int a = 0; a < APP_COUNT && nicons < MAX_ICONS; a++) {
        if (!app_visible(a) || g_apps[a].start_menu_only) continue;
        struct dicon *d = &icons[nicons++];
        memset(d, 0, sizeof *d);
        snprintf(d->key, sizeof d->key, "app:%s", g_apps[a].name);
        strlcpy(d->label, g_apps[a].name, sizeof d->label);
        d->icon = g_apps[a].icon;
        d->app = a;
    }
    if (!vfs_is_dir(DESKTOP_DIR)) nbfs_mkdir(ROOTFS, DESKTOP_DIR);
    struct vfs_item *items;
    int n = vfs_list(DESKTOP_DIR, &items);
    for (int i = 0; i < n && nicons < MAX_ICONS; i++) {
        struct dicon *d = &icons[nicons++];
        memset(d, 0, sizeof *d);
        snprintf(d->key, sizeof d->key, "file:%s", items[i].name);
        strlcpy(d->label, items[i].name, sizeof d->label);
        nbfs_path_join(DESKTOP_DIR, items[i].name, d->path, sizeof d->path);
        d->is_dir = items[i].type == NBFS_TYPE_DIR;
        d->icon = icon_for_file(items[i].name, d->is_dir);
        d->app = -1;
    }
    if (n > 0) kfree(items);
    apply_saved_positions();
    for (int i = 0; i < nicons; i++) if (!strcmp(icons[i].key, selected_key)) icons[i].selected = true;
    dir_signature = compute_signature();
    wm_damage(rect_make(0, 0, SCREEN_W, SCREEN_H - TASKBAR_H));
    klog("[desk] %d icons\n", nicons);
}

void desktop_screen_changed(void)
{
    load_wallpaper();
    use_prev = false;       /* re-place everything from the saved config */
    desktop_refresh();
    use_prev = true;
}

void desktop_init(void)
{
    load_desktop_cfg();
    load_wallpaper();
    desktop_refresh();
}

/* ---- drawing ------------------------------------------------------- */
static void draw_label(surface_t *s, struct dicon *d, int x, int y, bool sel)
{
    /* up to two centred lines (UI font), breaking at a space when possible,
     * ellipsis when the second line would overflow */
    int maxw = GRID_W - 2;
    char l1[NBFS_NAME_MAX + 1], l2[NBFS_NAME_MAX + 4];
    l2[0] = 0;
    int len = strlen(d->label);
    /* a single word that is only a little too wide stays on one line,
     * overflowing the cell slightly, rather than breaking mid-word */
    bool one_word = !strchr(d->label, ' ');
    if (gfx_ui_width(d->label) <= maxw || (one_word && gfx_ui_width(d->label) <= GRID_W + 16)) {
        strlcpy(l1, d->label, sizeof l1);
    } else {
        int fit = 0;
        while (fit < len && gfx_ui_width_n(d->label, fit + 1) <= maxw) fit++;
        int brk = fit;
        for (int i = fit; i > 2; i--) if (d->label[i] == ' ') { brk = i; break; }
        memcpy(l1, d->label, brk);
        l1[brk] = 0;
        const char *rest = d->label + brk + (d->label[brk] == ' ');
        if (gfx_ui_width(rest) <= maxw) strlcpy(l2, rest, sizeof l2);
        else {
            int n = strlen(rest);
            while (n > 0 && gfx_ui_width_n(rest, n) + gfx_ui_width("...") > maxw) n--;
            memcpy(l2, rest, n);
            strcpy(l2 + n, "...");
        }
    }
    int lines = l2[0] ? 2 : 1;
    int w1 = gfx_ui_width(l1), w2 = gfx_ui_width(l2), tw = MAX(w1, w2);
    if (sel) gfx_blend_rect(s, x + (GRID_W - tw) / 2 - 3, y + 40, tw + 6, lines * 16 + 2, C_SELECT, 200);
    const char *ls[2] = { l1, l2 };
    int ws[2] = { w1, w2 };
    for (int i = 0; i < lines; i++) {
        int tx = x + (GRID_W - ws[i]) / 2, ty = y + 41 + i * 16;
        if (!sel) gfx_ui_text(s, tx + 1, ty + 1, ls[i], 0x000000);
        gfx_ui_text(s, tx, ty, ls[i], 0xFFFFFF);
    }
}

static void draw_icon_at(surface_t *s, struct dicon *d, int x, int y)
{
    if (d->selected) gfx_blend_rect(s, x + (GRID_W - 40) / 2, y + 2, 40, 38, C_SELECT, 90);
    icon_draw(s, d->icon, x + (GRID_W - 32) / 2, y + 6);
    if (d->app >= 0) {        /* small shortcut arrow for application icons */
        int ax = x + (GRID_W - 32) / 2, ay = y + 6 + 22;
        gfx_fill_rect(s, ax, ay, 10, 10, 0xFFFFFF);
        gfx_rect(s, ax, ay, 10, 10, RGB(80, 80, 90));
        gfx_line(s, ax + 2, ay + 7, ax + 7, ay + 2, RGB(30, 90, 200));
        gfx_line(s, ax + 4, ay + 2, ax + 7, ay + 2, RGB(30, 90, 200));
        gfx_line(s, ax + 7, ay + 2, ax + 7, ay + 5, RGB(30, 90, 200));
    }
    draw_label(s, d, x, y, d->selected);
}

void desktop_draw(surface_t *s)
{
    rect_t area = rect_make(0, 0, SCREEN_W, SCREEN_H - TASKBAR_H);
    if (!rect_overlaps(area, s->clip)) return;
    if (wall) gfx_blit(s, 0, 0, wall, 0, 0, SCREEN_W, SCREEN_H);
    else gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, wall_color);
    for (int i = 0; i < nicons; i++) {
        if (dragging && i == press_icon) continue;
        if (rect_overlaps(rect_make(icons[i].x, icons[i].y, GRID_W, GRID_H), s->clip))
            draw_icon_at(s, &icons[i], icons[i].x, icons[i].y);
    }
}

void desktop_draw_overlay(surface_t *s)
{
    if (!dragging || press_icon < 0) return;
    struct dicon *d = &icons[press_icon];
    int x = drag_x - drag_off_x, y = drag_y - drag_off_y;
    if (!rect_overlaps(rect_make(x, y, GRID_W, GRID_H), s->clip)) return;
    draw_icon_at(s, d, x, y);
}

/* ---- actions ------------------------------------------------------- */
static int icon_at(int x, int y)
{
    for (int i = nicons - 1; i >= 0; i--) {
        struct dicon *d = &icons[i];
        rect_t hit = rect_make(d->x + (GRID_W - 56) / 2, d->y + 2, 56, GRID_H - 6);
        if (rect_contains(hit, x, y)) return i;
    }
    return -1;
}

static void select_only(int idx)
{
    for (int i = 0; i < nicons; i++) {
        bool sel = i == idx;
        if (icons[i].selected != sel) { icons[i].selected = sel; damage_icon(&icons[i]); }
    }
}

static void open_icon(int i)
{
    struct dicon *d = &icons[i];
    klog("[desk] open icon \"%s\"\n", d->label);
    if (d->app >= 0) app_launch(d->app, NULL);
    else app_open_path(d->path);
}

static char pending_path[NBFS_PATH_MAX];

static void rename_cb(bool ok, const char *text, void *ctx)
{
    if (!ok || !text[0]) return;
    char np[NBFS_PATH_MAX];
    nbfs_path_join(DESKTOP_DIR, text, np, sizeof np);
    if (strcmp(np, pending_path) == 0) return;
    /* keep the icon where it was: carry its position over to the new key */
    char oldkey[80], newkey[80];
    snprintf(oldkey, sizeof oldkey, "file:%s", nbfs_path_basename(pending_path));
    snprintf(newkey, sizeof newkey, "file:%s", text);
    int r = nbfs_rename(ROOTFS, pending_path, np);
    if (r) {
        char msg[160];
        snprintf(msg, sizeof msg, "Cannot rename: %s.", nbfs_strerror(r));
        msgbox(NULL, "Rename", msg, ICON_ERROR, MB_OK, NULL, NULL);
        return;
    }
    for (int i = 0; i < nicons; i++) if (!strcmp(icons[i].key, oldkey)) strlcpy(icons[i].key, newkey, sizeof icons[i].key);
    save_positions();
    desktop_refresh();
}

static void start_rename(const char *path)
{
    strlcpy(pending_path, path, sizeof pending_path);
    inputbox(NULL, "Rename", "Enter a new name:", nbfs_path_basename(path), rename_cb, NULL);
}

static void delete_cb(int r, void *ctx)
{
    if (r != DLG_YES) return;
    int e = nbfs_remove_tree(ROOTFS, pending_path);
    if (e) {
        char msg[160];
        snprintf(msg, sizeof msg, "Cannot delete: %s.", nbfs_strerror(e));
        msgbox(NULL, "Delete", msg, ICON_ERROR, MB_OK, NULL, NULL);
    }
    desktop_refresh();
}

static void start_delete(const char *path)
{
    strlcpy(pending_path, path, sizeof pending_path);
    char q[200];
    snprintf(q, sizeof q, "Are you sure you want to permanently delete \"%s\"?", nbfs_path_basename(path));
    msgbox(NULL, "Delete", q, ICON_WARNING, MB_YESNO, delete_cb, NULL);
}

static void file_props(const char *path)
{
    struct nbfs_stat st;
    if (nbfs_stat(ROOTFS, path, &st)) return;
    char sz[24], c[24], m[24], msg[400];
    vfs_format_size(st.size, sz, sizeof sz);
    vfs_format_time(st.ctime, c, sizeof c);
    vfs_format_time(st.mtime, m, sizeof m);
    snprintf(msg, sizeof msg, "Name: %s\nLocation: %s\nType: %s\nSize: %s\nCreated: %s\nModified: %s",
             nbfs_path_basename(path), DESKTOP_DIR, st.type == NBFS_TYPE_DIR ? "Folder" : "File", sz, c, m);
    msgbox(NULL, "Properties", msg, icon_for_file(path, st.type == NBFS_TYPE_DIR), MB_OK, NULL, NULL);
}

static int menu_x, menu_y;
enum { DM_NEW_FOLDER = 1, DM_NEW_TEXT, DM_WALLPAPER, DM_REFRESH, DM_PROPERTIES,
       IM_OPEN = 20, IM_RENAME, IM_DELETE, IM_PROPS };
static int menu_icon = -1;

static void place_icon_at(const char *name, int px, int py)
{
    char key[80];
    snprintf(key, sizeof key, "file:%s", name);
    desktop_refresh();
    for (int i = 0; i < nicons; i++) {
        if (strcmp(icons[i].key, key)) continue;
        int x = px - GRID_W / 2, y = py - GRID_H / 3;
        snap(&x, &y);
        icons[i].x = -1000;
        free_cell(&x, &y, i);
        icons[i].x = x; icons[i].y = y;
        select_only(i);
        save_positions();
        wm_damage(rect_make(0, 0, SCREEN_W, SCREEN_H - TASKBAR_H));
    }
}

/* remember where the user right-clicked so the new icon appears there */
static void place_new_icon_at_menu(const char *name) { place_icon_at(name, menu_x, menu_y); }

static void drop_error(const char *what, int r)
{
    char msg[200];
    snprintf(msg, sizeof msg, "Cannot %s: %s.", what, nbfs_strerror(r));
    msgbox(NULL, "Desktop", msg, ICON_ERROR, MB_OK, NULL, NULL);
}

/* Drop of a file/folder onto icon `target` (folder: move into it; app: open
 * the file with it).  Returns false if the target does not accept it. */
static bool drop_on_icon(int target, const char *path, bool copy)
{
    struct dicon *t = &icons[target];
    char out[NBFS_PATH_MAX];
    if (t->app >= 0) {
        if (vfs_is_dir(path) && t->app != APP_EXPLORER && t->app != APP_TERMINAL) return false;
        klog("[desk] open %s with %s\n", path, g_apps[t->app].name);
        app_launch(t->app, path);
        return true;
    }
    if (!t->is_dir || !strcmp(t->path, path)) return false;
    int r = vfs_move_into(path, t->path, copy, out, sizeof out);
    if (r) drop_error(copy ? "copy" : "move", r);
    else klog("[desk] %s %s -> %s\n", copy ? "copied" : "moved", path, out);
    desktop_refresh();
    return true;
}

void desktop_drop(const char *path, int x, int y, bool copy)
{
    int i = icon_at(x, y);
    if (i >= 0 && drop_on_icon(i, path, copy)) return;
    char out[NBFS_PATH_MAX];
    int r = vfs_move_into(path, DESKTOP_DIR, copy, out, sizeof out);
    if (r) { drop_error(copy ? "copy" : "move", r); return; }
    klog("[desk] %s %s -> %s\n", copy ? "copied" : "dropped", path, out);
    place_icon_at(nbfs_path_basename(out), x, y);
}

static void desk_menu_cb(int id, void *ctx)
{
    char path[NBFS_PATH_MAX];
    switch (id) {
    case DM_NEW_FOLDER:
        if (vfs_create_unique(DESKTOP_DIR, "New Folder", "", NBFS_TYPE_DIR, path, sizeof path) == 0) {
            klog("[desk] created %s\n", path);
            place_new_icon_at_menu(nbfs_path_basename(path));
            start_rename(path);
        }
        break;
    case DM_NEW_TEXT:
        if (vfs_create_unique(DESKTOP_DIR, "New Text Document", ".txt", NBFS_TYPE_FILE, path, sizeof path) == 0) {
            klog("[desk] created %s\n", path);
            place_new_icon_at_menu(nbfs_path_basename(path));
            start_rename(path);
        }
        break;
    case DM_WALLPAPER: wallpaper_chooser_open(); break;
    case DM_REFRESH: load_wallpaper(); desktop_refresh(); break;
    case DM_PROPERTIES: sysprops_open(); break;
    case IM_OPEN: if (menu_icon >= 0 && menu_icon < nicons) open_icon(menu_icon); break;
    case IM_RENAME: if (menu_icon >= 0 && icons[menu_icon].app < 0) start_rename(icons[menu_icon].path); break;
    case IM_DELETE: if (menu_icon >= 0 && icons[menu_icon].app < 0) start_delete(icons[menu_icon].path); break;
    case IM_PROPS:
        if (menu_icon >= 0 && icons[menu_icon].app < 0) file_props(icons[menu_icon].path);
        else sysprops_open();
        break;
    }
}

static void open_desktop_menu(int x, int y)
{
    static const menu_item_t items[] = {
        { "New Folder", DM_NEW_FOLDER, 0, ICON_FOLDER },
        { "New Text File", DM_NEW_TEXT, 0, ICON_FILE_TEXT },
        { "", 0, MENU_SEP, -1 },
        { "Change Wallpaper...", DM_WALLPAPER, 0, ICON_WALLPAPER },
        { "Refresh", DM_REFRESH, 0, ICON_RESTART },
        { "", 0, MENU_SEP, -1 },
        { "Properties", DM_PROPERTIES, 0, ICON_SETTINGS },
    };
    menu_x = x; menu_y = y;
    menu_open(x, y, items, ARRAY_SIZE(items), desk_menu_cb, NULL, false);
    klog("[desk] context menu opened\n");
}

static void open_icon_menu(int i, int x, int y)
{
    bool file = icons[i].app < 0;
    menu_item_t items[] = {
        { "Open", IM_OPEN, 0, -1 },
        { "", 0, MENU_SEP, -1 },
        { "Rename", IM_RENAME, file ? 0 : MENU_DISABLED, -1 },
        { "Delete", IM_DELETE, file ? 0 : MENU_DISABLED, -1 },
        { "", 0, MENU_SEP, -1 },
        { "Properties", IM_PROPS, 0, -1 },
    };
    menu_icon = i;
    menu_open(x, y, items, ARRAY_SIZE(items), desk_menu_cb, NULL, false);
}

/* ---- input --------------------------------------------------------- */
void desktop_mouse_down(int x, int y, int button, bool dbl)
{
    int i = icon_at(x, y);
    if (button == MOUSE_LEFT) {
        select_only(i);
        press_icon = i;
        press_x = x; press_y = y;
        dragging = false;
        if (i >= 0) {
            drag_off_x = x - icons[i].x;
            drag_off_y = y - icons[i].y;
            if (dbl) { press_icon = -1; open_icon(i); }
        }
    }
}

void desktop_mouse_move(int x, int y, u8 buttons)
{
    if (!(buttons & MOUSE_LEFT) || press_icon < 0) return;
    if (!dragging && ABS(x - press_x) + ABS(y - press_y) > 4) {
        dragging = true;
        damage_icon(&icons[press_icon]);
    }
    if (dragging) {
        wm_damage(rect_make(drag_x - drag_off_x, drag_y - drag_off_y, GRID_W, GRID_H));
        drag_x = x; drag_y = y;
        wm_damage(rect_make(drag_x - drag_off_x, drag_y - drag_off_y, GRID_W, GRID_H));
    }
}

void desktop_mouse_up(int x, int y, int button)
{
    if (button == MOUSE_RIGHT) {
        int i = icon_at(x, y);
        if (i >= 0) { select_only(i); open_icon_menu(i, x, y); }
        else { select_only(-1); open_desktop_menu(x, y); }
        return;
    }
    if (button != MOUSE_LEFT) return;
    if (dragging && press_icon >= 0) {
        struct dicon *d = &icons[press_icon];
        bool copy = keyboard_mods() & MOD_CTRL;
        window_t *w = wm_window_under(x, y);
        int target = icon_at(x, y);
        if (w || (target >= 0 && target != press_icon)) {
            wm_damage(rect_make(drag_x - drag_off_x, drag_y - drag_off_y, GRID_W, GRID_H));
            damage_icon(d);
            dragging = false;
            int src = press_icon;
            press_icon = -1;
            if (icons[src].app < 0) {
                char path[NBFS_PATH_MAX];
                strlcpy(path, icons[src].path, sizeof path);
                if (w) wm_send_drop(w, path, x, y, copy);
                else drop_on_icon(target, path, copy);
            }
            return;
        }
        wm_damage(rect_make(drag_x - drag_off_x, drag_y - drag_off_y, GRID_W, GRID_H));
        int nx = x - drag_off_x, ny = y - drag_off_y;
        snap(&nx, &ny);
        free_cell(&nx, &ny, press_icon);
        damage_icon(d);
        d->x = nx; d->y = ny;
        damage_icon(d);
        klog("[desk] icon \"%s\" moved to %d,%d\n", d->label, nx, ny);
        save_positions();
    }
    dragging = false;
    press_icon = -1;
}

void desktop_key(struct key_event *ke)
{
    if (!ke->pressed) return;
    int sel = -1;
    for (int i = 0; i < nicons; i++) if (icons[i].selected) sel = i;
    if (ke->key == KEY_F5) { desktop_refresh(); return; }
    if (sel < 0) {
        if (nicons && (ke->key == KEY_DOWN || ke->key == KEY_RIGHT || ke->key == KEY_TAB)) select_only(0);
        return;
    }
    if (ke->key == KEY_ENTER) open_icon(sel);
    else if (ke->key == KEY_DELETE && icons[sel].app < 0) start_delete(icons[sel].path);
    else if (ke->key == KEY_F2 && icons[sel].app < 0) start_rename(icons[sel].path);
    else if (ke->key == KEY_DOWN || ke->key == KEY_UP || ke->key == KEY_LEFT || ke->key == KEY_RIGHT) {
        int dx = ke->key == KEY_RIGHT ? GRID_W : ke->key == KEY_LEFT ? -GRID_W : 0;
        int dy = ke->key == KEY_DOWN ? GRID_H : ke->key == KEY_UP ? -GRID_H : 0;
        int tx = icons[sel].x + dx, ty = icons[sel].y + dy;
        for (int i = 0; i < nicons; i++) if (icons[i].x == tx && icons[i].y == ty) { select_only(i); break; }
    }
}

void desktop_tick(void)
{
    static u64 last;
    u64 now = timer_ticks();
    if (now - last < 1500 || dragging) return;
    last = now;
    if (compute_signature() != dir_signature) desktop_refresh();
}

/* ======================================================================
 * Wallpaper chooser
 * ==================================================================== */
#define WP_MAX 24
static window_t *wp_win;
static struct { char label[48]; char path[NBFS_PATH_MAX]; u32 color; } wp_opts[WP_MAX];
static int wp_n, wp_sel;
static rect_t wp_close;

static void wp_scan(void)
{
    static const struct { const char *n; u32 c; } solids[] = {
        { "Solid: Teal", C_DESKTOP_DEFAULT }, { "Solid: Midnight", RGB(20, 28, 54) },
        { "Solid: Forest", RGB(34, 85, 51) }, { "Solid: Plum", RGB(86, 44, 92) }, { "Solid: Slate", RGB(70, 80, 96) },
    };
    wp_n = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(solids); i++) {
        strlcpy(wp_opts[wp_n].label, solids[i].n, sizeof wp_opts[0].label);
        wp_opts[wp_n].path[0] = 0;
        wp_opts[wp_n].color = solids[i].c;
        wp_n++;
    }
    const char *dirs[] = { "/System/Wallpapers", "/Pictures" };
    for (int d = 0; d < 2; d++) {
        struct vfs_item *items;
        int n = vfs_list(dirs[d], &items);
        for (int i = 0; i < n && wp_n < WP_MAX; i++) {
            const char *dot = strrchr(items[i].name, '.');
            if (items[i].type != NBFS_TYPE_FILE || !dot || strcasecmp(dot, ".bmp")) continue;
            snprintf(wp_opts[wp_n].label, sizeof wp_opts[0].label, "%s", items[i].name);
            nbfs_path_join(dirs[d], items[i].name, wp_opts[wp_n].path, sizeof wp_opts[0].path);
            wp_opts[wp_n].color = wall_color;
            wp_n++;
        }
        if (n > 0) kfree(items);
    }
    wp_sel = -1;
    for (int i = 0; i < wp_n; i++) {
        if (wall && wp_opts[i].path[0] && !strcmp(wp_opts[i].path, wall_path)) wp_sel = i;
        if (!wall && !wp_opts[i].path[0] && wp_opts[i].color == wall_color) wp_sel = i;
    }
}

static void wp_paint(window_t *w)
{
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_WINDOW_BG);
    gfx_text(s, 14, 10, "Choose a wallpaper (applies immediately):", C_TEXT, COLOR_NONE);
    ui_sunken(s, rect_make(12, 32, 230, s->h - 84), 0xFFFFFF);
    for (int i = 0; i < wp_n; i++) {
        int y = 34 + i * 22;
        if (i == wp_sel) gfx_fill_rect(s, 14, y, 226, 22, C_SELECT);
        if (!wp_opts[i].path[0]) gfx_fill_rect(s, 20, y + 4, 14, 14, wp_opts[i].color);
        else icon_draw_small(s, ICON_FILE_IMAGE, 19, y + 3);
        gfx_text(s, 42, y + 3, wp_opts[i].label, i == wp_sel ? 0xFFFFFF : C_TEXT, COLOR_NONE);
    }
    /* live preview of the current desktop background */
    rect_t pv = rect_make(258, 32, s->w - 270, (s->w - 270) * 3 / 4);
    gfx_fill_rect(s, pv.x - 6, pv.y - 6, pv.w + 12, pv.h + 22, RGB(60, 60, 70));
    if (wall) gfx_blit_scaled(s, pv, wall);
    else gfx_fill_rect(s, pv.x, pv.y, pv.w, pv.h, wall_color);
    gfx_fill_rect(s, pv.x + pv.w / 2 - 20, pv.y + pv.h + 6, 40, 6, RGB(90, 90, 100));
    wp_close = rect_make(s->w - 100, s->h - 40, 84, 28);
    ui_button(s, wp_close, "Close", BTN_DEFAULT);
    wm_invalidate(w);
}

static void wp_event(window_t *w, event_t *e)
{
    switch (e->type) {
    case EV_PAINT: wp_paint(w); break;
    case EV_MOUSE_DOWN:
        if (rect_contains(rect_make(14, 34, 226, wp_n * 22), e->x, e->y)) {
            int i = (e->y - 34) / 22;
            wp_sel = i;
            klog("[desk] wallpaper -> %s\n", wp_opts[i].label);
            desktop_set_wallpaper(wp_opts[i].path[0] ? wp_opts[i].path : "none", wp_opts[i].color);
            wp_paint(w);
        }
        break;
    case EV_MOUSE_UP:
        if (rect_contains(wp_close, e->x, e->y)) wm_destroy_window(w);
        break;
    case EV_KEY:
        if (e->key.pressed && (e->key.key == KEY_ESC || e->key.key == KEY_ENTER)) wm_destroy_window(w);
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: wp_win = NULL; break;
    }
}

void wallpaper_chooser_open(void)
{
    if (wm_window_alive(wp_win)) { wm_focus(wp_win); return; }
    wp_scan();
    wp_win = wm_create("Change Wallpaper", ICON_WALLPAPER, 560, 340, WF_DIALOG, wp_event, NULL);
    if (wp_win) wm_center(wp_win);
}
