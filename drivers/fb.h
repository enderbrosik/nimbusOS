#ifndef NIMBUS_FB_H
#define NIMBUS_FB_H
#include "kernel/types.h"
struct framebuffer {
    u32 *pixels;        /* virtual address (VMM_FB_BASE) */
    u32 phys;
    int width, height;
    int pitch;          /* in pixels */
};
extern struct framebuffer g_fb;
bool fb_init(int w, int h);
#endif
