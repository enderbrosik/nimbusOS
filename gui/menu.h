#ifndef NIMBUS_MENU_H
#define NIMBUS_MENU_H
#include "gui/gfx.h"
#include "drivers/keyboard.h"

#define MENU_SEP      1
#define MENU_DISABLED 2
#define MENU_MAX_ITEMS 24

typedef struct {
    const char *label;
    int id;
    int flags;
    int icon;            /* ICON_* or -1 */
} menu_item_t;

typedef void (*menu_cb_t)(int id, void *ctx);

/* Open a popup menu with its top-left corner at (x,y) (moved to stay on
 * screen).  banner: draws the Start menu side banner. */
void   menu_open(int x, int y, const menu_item_t *items, int n, menu_cb_t cb, void *ctx, bool banner);
void   menu_open_above(int x, int bottom_y, const menu_item_t *items, int n, menu_cb_t cb, void *ctx, bool banner);
void   menu_close(void);
bool   menu_is_open(void);
bool   menu_is_banner(void);
rect_t menu_rect(void);
void   menu_draw(surface_t *s);
/* input; return true when consumed */
bool   menu_mouse_move(int x, int y);
bool   menu_mouse_down(int x, int y);
bool   menu_mouse_up(int x, int y);
bool   menu_key(struct key_event *ke);
#endif
