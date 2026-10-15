/*
 * NimbusOS - PS/2 keyboard driver, scancode set 1 (the controller
 * translates the keyboard's set 2 into set 1, see drivers/ps2.c).
 * Scancode tables: OSDev wiki "PS/2 Keyboard#Scan Code Set 1".
 * Extended keys are prefixed with 0xE0; bit 7 set = key release.
 */
#include "drivers/keyboard.h"
#include "drivers/ps2.h"
#include "arch/x86/io.h"
#include "arch/x86/idt.h"
#include "kernel/kprintf.h"

#define QSIZE 128
static struct key_event queue[QSIZE];
static volatile u32 qhead, qtail;
static u8 mods;
static bool lshift, rshift, lctrl, rctrl, lalt, ralt, lwin, rwin, extended;

static const u16 set1[0x59] = {
    0, KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', KEY_BACKSPACE, KEY_TAB,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', KEY_ENTER, KEY_LCTRL, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', KEY_LSHIFT, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT, '*', KEY_LALT, ' ', KEY_CAPS, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_NUMLOCK, KEY_SCROLL,
    /* keypad 0x47-0x53 (treated as navigation keys, num lock ignored) */
    KEY_HOME, KEY_UP, KEY_PGUP, '-', KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END, KEY_DOWN, KEY_PGDN, KEY_INSERT, KEY_DELETE,
    0, 0, 0, KEY_F11, KEY_F12,
};

static const char shifted_digits[] = ")!@#$%^&*(";

static char translate(u16 key)
{
    bool shift = mods & MOD_SHIFT;
    if (key >= 'a' && key <= 'z') {
        bool upper = shift ^ ((mods & MOD_CAPS) != 0);
        return upper ? (char)(key - 32) : (char)key;
    }
    if (key == KEY_ENTER) return '\n';
    if (key == KEY_TAB) return '\t';
    if (key == KEY_BACKSPACE) return '\b';
    if (key >= 0x100 || key == KEY_ESC) return 0;
    if (!shift) return (char)key;
    if (key >= '0' && key <= '9') return shifted_digits[key - '0'];
    switch (key) {
    case '-': return '_';  case '=': return '+';  case '[': return '{';  case ']': return '}';
    case ';': return ':';  case '\'': return '"'; case '`': return '~';  case '\\': return '|';
    case ',': return '<';  case '.': return '>';  case '/': return '?';
    default: return (char)key;
    }
}

static void update_mods(void)
{
    u8 m = mods & MOD_CAPS;
    if (lshift || rshift) m |= MOD_SHIFT;
    if (lctrl || rctrl) m |= MOD_CTRL;
    if (lalt || ralt) m |= MOD_ALT;
    if (lwin || rwin) m |= MOD_WIN;
    mods = m;
}

static void push(u16 key, bool pressed)
{
    struct key_event ev = { key, pressed ? translate(key) : 0, pressed, mods };
    u32 next = (qhead + 1) % QSIZE;
    if (next == qtail) return;      /* queue full: drop */
    queue[qhead] = ev;
    qhead = next;
}

static void kbd_irq(struct regs *r)
{
    (void)r;
    u8 sc = inb(PS2_DATA);
    if (sc == 0xE0) { extended = true; return; }
    if (sc == 0xE1) return;         /* Pause key sequence: ignored */
    bool released = sc & 0x80;
    sc &= 0x7F;
    u16 key = 0;
    if (extended) {
        extended = false;
        switch (sc) {
        case 0x1C: key = KEY_ENTER; break;
        case 0x1D: key = KEY_RCTRL; break;
        case 0x35: key = '/'; break;
        case 0x38: key = KEY_RALT; break;
        case 0x47: key = KEY_HOME; break;
        case 0x48: key = KEY_UP; break;
        case 0x49: key = KEY_PGUP; break;
        case 0x4B: key = KEY_LEFT; break;
        case 0x4D: key = KEY_RIGHT; break;
        case 0x4F: key = KEY_END; break;
        case 0x50: key = KEY_DOWN; break;
        case 0x51: key = KEY_PGDN; break;
        case 0x52: key = KEY_INSERT; break;
        case 0x53: key = KEY_DELETE; break;
        case 0x5B: key = KEY_LWIN; break;
        case 0x5C: key = KEY_RWIN; break;
        case 0x5D: key = KEY_MENU; break;
        case 0x2A: case 0x37: return;   /* fake shifts around PrintScreen */
        default: return;
        }
    } else if (sc < sizeof set1 / sizeof set1[0]) {
        key = set1[sc];
    }
    if (!key) return;
    switch (key) {
    case KEY_LSHIFT: lshift = !released; break;
    case KEY_RSHIFT: rshift = !released; break;
    case KEY_LCTRL:  lctrl = !released; break;
    case KEY_RCTRL:  rctrl = !released; break;
    case KEY_LALT:   lalt = !released; break;
    case KEY_RALT:   ralt = !released; break;
    case KEY_LWIN:   lwin = !released; break;
    case KEY_RWIN:   rwin = !released; break;
    case KEY_CAPS:   if (!released) mods ^= MOD_CAPS; break;
    }
    update_mods();
    push(key, !released);
}

void keyboard_init(void)
{
    irq_register(1, kbd_irq);
    kprintf("[kbd ] PS/2 keyboard ready (scancode set 1)\n");
}

bool keyboard_poll(struct key_event *ev)
{
    u32 f = irq_save();
    bool ok = qtail != qhead;
    if (ok) { *ev = queue[qtail]; qtail = (qtail + 1) % QSIZE; }
    irq_restore(f);
    return ok;
}

u8 keyboard_mods(void) { return mods; }
