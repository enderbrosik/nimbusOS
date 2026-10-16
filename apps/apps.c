/* NimbusOS - application registry and file-type associations. */
#include "apps/apps.h"
#include "gui/icons.h"
#include "gui/dialog.h"
#include "fs/vfs.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

const struct app_def g_apps[APP_COUNT] = {
    [APP_EXPLORER]  = { "File Explorer", ICON_EXPLORER, explorer_open, false },
    [APP_NOTEPAD]   = { "Notepad", ICON_NOTEPAD, notepad_open, false },
    [APP_PAINT]     = { "Paint", ICON_PAINT, paint_open, false },
    [APP_TERMINAL]  = { "Terminal", ICON_TERMINAL, terminal_open, false },
    [APP_TASKMGR]   = { "Task Manager", ICON_TASKMGR, taskmgr_open, false },
    [APP_INSTALLER] = { "Install NimbusOS", ICON_INSTALLER, installer_open, false },
    [APP_MINES]     = { "Minesweeper", ICON_MINES, minesweeper_open, false },
    [APP_SNAKE]     = { "Snake", ICON_SNAKE, snake_open, false },
    [APP_SOLITAIRE] = { "Solitaire", ICON_CARDS, solitaire_open, false },
    [APP_SETTINGS]  = { "Settings", ICON_SETTINGS, settings_open, true },
};

bool system_is_live(void) { return !vfs_exists("/System/installed.txt"); }

bool app_visible(int app)
{
    if (app == APP_INSTALLER) return system_is_live();
    return app >= 0 && app < APP_COUNT;
}

void app_launch(int app, const char *arg)
{
    if (app < 0 || app >= APP_COUNT) return;
    klog("[app ] launching %s%s%s\n", g_apps[app].name, arg ? " " : "", arg ? arg : "");
    g_apps[app].launch(arg);
}

void app_open_path(const char *path)
{
    if (vfs_is_dir(path)) { app_launch(APP_EXPLORER, path); return; }
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".bmp")) app_launch(APP_PAINT, path);
    else app_launch(APP_NOTEPAD, path);
}
