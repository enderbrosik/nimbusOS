#ifndef NIMBUS_DESKTOP_H
#define NIMBUS_DESKTOP_H
#include "gui/gfx.h"
#include "drivers/keyboard.h"
void desktop_init(void);
void desktop_refresh(void);
void desktop_draw(surface_t *s);
void desktop_draw_overlay(surface_t *s);
void desktop_mouse_down(int x, int y, int button, bool dbl);
void desktop_mouse_move(int x, int y, u8 buttons);
void desktop_mouse_up(int x, int y, int button);
void desktop_key(struct key_event *ke);
void desktop_tick(void);
void desktop_set_wallpaper(const char *path, u32 color);
void wallpaper_chooser_open(void);
#endif
