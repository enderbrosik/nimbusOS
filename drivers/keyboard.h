#ifndef NIMBUS_KEYBOARD_H
#define NIMBUS_KEYBOARD_H
#include "kernel/types.h"

/* Key codes: printable keys use their (unshifted) ASCII value, others: */
enum {
    KEY_BACKSPACE = 8, KEY_TAB = 9, KEY_ENTER = 10, KEY_ESC = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL,
    KEY_LALT, KEY_RALT, KEY_CAPS, KEY_LWIN, KEY_RWIN, KEY_MENU, KEY_NUMLOCK, KEY_SCROLL,
};

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_WIN   8
#define MOD_CAPS  16

struct key_event {
    u16 key;        /* KEY_* or ASCII (lower case for letters) */
    char ch;        /* translated character, 0 if none */
    u8 pressed;     /* 1 = make, 0 = break */
    u8 mods;        /* MOD_* at the time of the event */
};

void keyboard_init(void);
bool keyboard_poll(struct key_event *ev);
u8   keyboard_mods(void);
#endif
