#ifndef NIMBUS_ICONS_H
#define NIMBUS_ICONS_H
#include "gui/gfx.h"
enum {
    ICON_NONE = -1,
    ICON_FOLDER, ICON_FILE_TEXT, ICON_FILE_IMAGE, ICON_FILE, ICON_EXPLORER, ICON_NOTEPAD,
    ICON_PAINT, ICON_TERMINAL, ICON_TASKMGR, ICON_INSTALLER, ICON_COMPUTER, ICON_SHUTDOWN,
    ICON_RESTART, ICON_SETTINGS, ICON_DRIVE, ICON_WALLPAPER, ICON_INFO, ICON_WARNING, ICON_ERROR,
    ICON_MINES, ICON_SNAKE, ICON_CARDS, ICON_AUDIO, ICON_SPEAKER, ICON_MUTED, ICON_WEB, ICON_NETWORK,
    ICON_UPDATE, ICON_BLOCKRUN, ICON_BREAKOUT, ICON_COUNT
};
void icons_init(void);
void icon_draw(surface_t *s, int icon, int x, int y);       /* 32x32 */
void icon_draw_small(surface_t *s, int icon, int x, int y); /* 16x16 */
int  icon_for_file(const char *name, bool is_dir);
#endif
