/* NimbusOS - window manager / compositor API used by the desktop and apps. */
#ifndef NIMBUS_WM_H
#define NIMBUS_WM_H
#include "gui/gfx.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"

enum {
    EV_PAINT = 1,       /* surface content must be (re)drawn (after create/resize) */
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,
    EV_DBLCLICK,
    EV_KEY,
    EV_RESIZE,
    EV_CLOSE,           /* user asked to close; call wm_destroy_window to accept */
    EV_FOCUS,
    EV_BLUR,
    EV_TIMER,
    EV_DESTROY,         /* window is being destroyed: free app state */
};

typedef struct event {
    int type;
    int x, y;           /* client coordinates for mouse events */
    int button;         /* MOUSE_LEFT/RIGHT/MIDDLE for down/up */
    u8 buttons;         /* buttons held */
    struct key_event key;
} event_t;

#define WF_RESIZABLE   1
#define WF_NO_TASKBAR  2
#define WF_DIALOG      4    /* only a close button */
#define WF_NO_MAXIMIZE 8

typedef struct window window_t;
typedef void (*wm_handler_t)(window_t *w, event_t *e);

struct window {
    int id;
    char title[64];
    int icon;
    rect_t frame;           /* outer rectangle on screen */
    rect_t restore;         /* frame before maximize */
    surface_t *surface;     /* client area pixels */
    int flags;
    bool minimized, maximized, destroyed;
    int min_w, min_h;       /* minimum client size */
    wm_handler_t handler;
    void *app;              /* application state */
    window_t *owner;        /* dialogs: the window that opened them */
    window_t *modal;        /* dialog currently blocking this window */
    u32 timer_ms;
    u64 timer_next;
    u32 mem_hint;           /* extra memory the app reports for Task Manager */
};

/* lifecycle */
void      wm_init(void);
void      wm_run(void) NORETURN;
window_t *wm_create(const char *title, int icon, int client_w, int client_h, int flags,
                    wm_handler_t handler, void *app);
void      wm_destroy_window(window_t *w);
void      wm_close(window_t *w);                 /* sends EV_CLOSE */
void      wm_set_modal(window_t *owner, window_t *dialog);

/* geometry / state */
rect_t    wm_client_rect(window_t *w);           /* in screen coordinates */
void      wm_move(window_t *w, int x, int y);
void      wm_center(window_t *w);
void      wm_set_frame(window_t *w, rect_t frame);
void      wm_minimize(window_t *w);
void      wm_restore(window_t *w);
void      wm_toggle_maximize(window_t *w);
void      wm_focus(window_t *w);
window_t *wm_focused(void);
void      wm_set_title(window_t *w, const char *title);
void      wm_set_timer(window_t *w, u32 ms);

/* painting: apps draw into w->surface then invalidate */
void      wm_invalidate(window_t *w);
void      wm_invalidate_rect(window_t *w, rect_t client_r);
void      wm_damage(rect_t screen_r);             /* mark a screen area for redraw */

/* enumeration (taskbar, task manager, alt-tab) */
int       wm_window_count(void);
window_t *wm_window_at(int z);                   /* 0 = bottom */
bool      wm_window_alive(window_t *w);

/* screen / cursor */
int       wm_mouse_x(void);
int       wm_mouse_y(void);
enum { CURSOR_ARROW, CURSOR_TEXT, CURSOR_RESIZE_H, CURSOR_RESIZE_V, CURSOR_RESIZE_NWSE, CURSOR_RESIZE_NESW, CURSOR_CROSS };
void      wm_set_app_cursor(int cursor);          /* cursor while hovering a client */
u64       wm_ticks(void);
#endif
