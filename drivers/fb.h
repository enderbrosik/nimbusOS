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
bool fb_set_mode(int w, int h);            /* change resolution (after fb_init) */
bool fb_mode_supported(int w, int h);
u32  fb_vram_bytes(void);
#define FB_MAX_W 1920
#define FB_MAX_H 1200
#endif
