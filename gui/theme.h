/* NimbusOS - shared GUI metrics and colours. */
#ifndef NIMBUS_THEME_H
#define NIMBUS_THEME_H
#include "gui/gfx.h"

/* Screen size and accent colours are runtime settings (gui/theme.c). */
extern int g_screen_w, g_screen_h;
extern u32 g_accent, g_title1, g_title2, g_title_border, g_accent_light;
#define SCREEN_W g_screen_w
#define SCREEN_H g_screen_h
void theme_set_accent(u32 color);

#define TITLE_H     26
#define BORDER      1
#define SHADOW      6
#define TASKBAR_H   36
#define RESIZE_OUT  4      /* resize grab zone outside the frame */
#define RESIZE_IN   3      /* ... and inside it */

#define C_TITLE_ACTIVE_1   g_title1
#define C_TITLE_ACTIVE_2   g_title2
#define C_TITLE_INACTIVE_1 RGB(120, 126, 140)
#define C_TITLE_INACTIVE_2 RGB(156, 162, 176)
#define C_BORDER_ACTIVE    g_title_border
#define C_BORDER_INACTIVE  RGB(100, 104, 116)
#define C_WINDOW_BG        RGB(240, 240, 240)
#define C_TEXT             RGB(20, 20, 24)
#define C_TEXT_DIM         RGB(110, 110, 120)
#define C_SELECT           g_accent
#define C_SELECT_LIGHT     g_accent_light
#define ACCENT_DEFAULT     RGB(0, 120, 215)
#define C_BUTTON           RGB(225, 225, 228)
#define C_BUTTON_HOVER     RGB(229, 241, 251)
#define C_BUTTON_BORDER    RGB(173, 173, 180)
#define C_TASKBAR          RGB(28, 32, 42)
#define C_TASKBAR_BTN      RGB(52, 58, 74)
#define C_TASKBAR_BTN_ACT  RGB(78, 90, 120)
#define C_TASKBAR_TEXT     RGB(235, 238, 245)
#define C_MENU_BG          RGB(250, 250, 252)
#define C_MENU_BORDER      RGB(150, 150, 160)
#define C_DESKTOP_DEFAULT  RGB(0, 99, 132)
#endif
