#ifndef NIMBUS_TASKBAR_H
#define NIMBUS_TASKBAR_H
#include "gui/gfx.h"
#include "drivers/rtc.h"
void   taskbar_init(void);
rect_t taskbar_rect(void);
rect_t taskbar_start_rect(void);
void   taskbar_draw(surface_t *s);
void   taskbar_changed(void);
void   taskbar_mouse_down(int x, int y, int button);
void   taskbar_mouse_move(int x, int y);
void   taskbar_tick(void);
void   taskbar_toggle_start(void);
void   taskbar_shutdown_prompt(void);
void   system_shutdown(void);
void   system_restart(void);
void   clock_now(struct rtc_time *t);
#endif
