#ifndef NIMBUS_MOUSE_H
#define NIMBUS_MOUSE_H
#include "kernel/types.h"
#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4
struct mouse_event {
    int x, y;           /* absolute, clamped to the bounds */
    int dx, dy;
    u8 buttons;         /* MOUSE_* currently held */
    int wheel;          /* wheel clicks: > 0 = towards the user (scroll down) */
};
void mouse_init(void);
void mouse_set_bounds(int w, int h);
void mouse_set_speed(int quarters);      /* 4 = 1:1, 2 = half, 8 = double */
bool mouse_poll(struct mouse_event *ev);
void mouse_get(int *x, int *y, u8 *buttons);
#endif
