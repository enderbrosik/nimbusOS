/* NimbusOS - small immediate-mode widget helpers for applications. */
#ifndef NIMBUS_WIDGETS_H
#define NIMBUS_WIDGETS_H
#include "gui/gfx.h"
#include "drivers/keyboard.h"

enum { BTN_NORMAL, BTN_HOVER, BTN_PRESSED, BTN_DEFAULT, BTN_DISABLED };

void ui_button(surface_t *s, rect_t r, const char *label, int state);
void ui_icon_button(surface_t *s, rect_t r, int icon, const char *label, int state);
void ui_sunken(surface_t *s, rect_t r, u32 fill);          /* white edit-box style panel */
void ui_checkbox(surface_t *s, int x, int y, bool checked, const char *label);
void ui_radio(surface_t *s, int x, int y, bool checked, const char *label);
void ui_progress(surface_t *s, rect_t r, int percent);
void ui_toolbar(surface_t *s, rect_t r);
void ui_separator_v(surface_t *s, int x, int y, int h);
/* vertical scrollbar; returns the thumb rectangle */
rect_t ui_vscroll(surface_t *s, rect_t r, int total, int visible, int offset);
int    ui_vscroll_offset_for(rect_t r, int total, int visible, int thumb_y);
void   ui_text_wrapped(surface_t *s, rect_t r, const char *text, u32 fg);
int    ui_text_wrapped_height(const char *text, int width);

/* A simple button list for apps: hit-test by id. */
typedef struct { rect_t r; int id; const char *label; int icon; bool disabled; } ui_btn_t;
int  ui_btn_hit(const ui_btn_t *b, int n, int x, int y);   /* id or -1 */

/* Single line text field */
typedef struct {
    char text[256];
    int len, cursor, sel, scroll;    /* sel = selection anchor (== cursor when none) */
} textfield_t;
void tf_set(textfield_t *tf, const char *s);
bool tf_key(textfield_t *tf, struct key_event *ke);   /* true if handled */
void tf_draw(surface_t *s, rect_t r, textfield_t *tf, bool focused);
void tf_click(textfield_t *tf, rect_t r, int x);
#endif
