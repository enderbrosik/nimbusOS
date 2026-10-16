/* NimbusOS - application registry. */
#ifndef NIMBUS_APPS_H
#define NIMBUS_APPS_H
#include "kernel/types.h"

enum { APP_EXPLORER, APP_NOTEPAD, APP_PAINT, APP_TERMINAL, APP_TASKMGR, APP_INSTALLER,
       APP_MINES, APP_SNAKE, APP_SOLITAIRE, APP_SETTINGS, APP_COUNT };

struct app_def {
    const char *name;
    int icon;
    void (*launch)(const char *arg);
    bool start_menu_only;       /* no desktop icon */
};
extern const struct app_def g_apps[APP_COUNT];

void app_launch(int app, const char *arg);
bool app_visible(int app);          /* e.g. the installer only shows on the live system */
void app_open_path(const char *path);
bool system_is_live(void);

void explorer_open(const char *path);
void notepad_open(const char *path);
void paint_open(const char *path);
void terminal_open(const char *cwd);
void taskmgr_open(const char *unused);
void installer_open(const char *unused);
void minesweeper_open(const char *arg);
void snake_open(const char *arg);
void solitaire_open(const char *arg);
void settings_open(const char *page);
void sysprops_open(void);
#endif
