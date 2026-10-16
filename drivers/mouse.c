/*
 * NimbusOS - PS/2 mouse driver.
 * Standard 3-byte packets (OSDev wiki "PS/2 Mouse"):
 *   byte 0: Yovf Xovf Ysign Xsign 1 Mid Right Left
 *   byte 1: X movement (9-bit two's complement with Xsign)
 *   byte 2: Y movement (positive = up)
 * Bit 3 of byte 0 is always 1 and is used to resynchronise.
 *
 * Wheel: the IntelliMouse extension is unlocked by setting the sample rate
 * to 200, 100, 80 in a row; the device ID then reads 3 and every packet
 * gains a 4th byte with the wheel movement (signed, low 4 bits).
 * (Microsoft "Mouse and Keyboard Interface" spec; OSDev wiki "PS/2 Mouse").
 */
#include "drivers/mouse.h"
#include "drivers/ps2.h"
#include "arch/x86/io.h"
#include "arch/x86/idt.h"
#include "kernel/kprintf.h"

#define QSIZE 128
static struct mouse_event queue[QSIZE];
static volatile u32 qhead, qtail;
static u8 packet[3];
static int cycle;
static int mx, my, maxw = 1024, maxh = 768;
static u8 buttons;
static int speed = 4, sx_acc, sy_acc;   /* speed in quarters: 4 = 1:1 */
static int packet_len = 3;
static u8 packet4[4];

static void mouse_cmd(u8 cmd)
{
    ps2_write_cmd(0xD4);        /* next data byte goes to the aux port */
    ps2_write_data(cmd);
    ps2_read_data();            /* ACK (0xFA) */
}

static bool last_is_motion;

static void push(int dx, int dy, int wz, bool button_change)
{
    u32 last = (qhead + QSIZE - 1) % QSIZE;
    /* Coalesce pure motion events so a slow consumer cannot overflow the
     * queue; events carrying a button change are never merged, so clicks
     * keep their exact position. */
    if (!button_change && last_is_motion && qhead != qtail) {
        queue[last].x = mx; queue[last].y = my;
        queue[last].dx += dx; queue[last].dy += dy;
        queue[last].wheel += wz;
        return;
    }
    u32 next = (qhead + 1) % QSIZE;
    if (next == qtail) return;
    queue[qhead] = (struct mouse_event){ mx, my, dx, dy, buttons, wz };
    qhead = next;
    last_is_motion = !button_change;
}

static void mouse_irq(struct regs *r)
{
    (void)r;
    u8 st = inb(PS2_STATUS);
    if (!(st & 1)) return;
    u8 b = inb(PS2_DATA);
    if (cycle == 0 && !(b & 0x08)) return;      /* out of sync */
    packet4[cycle++] = b;
    if (cycle < packet_len) return;
    cycle = 0;
    for (int i = 0; i < 3; i++) packet[i] = packet4[i];
    int wz = 0;
    if (packet_len == 4) { wz = packet4[3] & 0x0F; if (wz & 0x08) wz -= 16; }
    if (packet[0] & 0xC0) return;               /* overflow: discard */
    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);
    dy = -dy;                                   /* screen Y grows downwards */
    /* pointer speed: scale by speed/4 keeping the remainder so slow
     * movements are not lost */
    sx_acc += dx * speed;
    sy_acc += dy * speed;
    int mdx = sx_acc / 4, mdy = sy_acc / 4;
    sx_acc -= mdx * 4;
    sy_acc -= mdy * 4;
    mx += mdx; my += mdy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= maxw) mx = maxw - 1;
    if (my >= maxh) my = maxh - 1;
    u8 nb = packet[0] & 7;
    bool changed = nb != buttons;
    buttons = nb;
    push(dx, dy, wz, changed);
}

void mouse_init(void)
{
    mouse_cmd(0xF6);            /* set defaults */
    static const u8 knock[3] = { 200, 100, 80 };   /* IntelliMouse unlock */
    for (int i = 0; i < 3; i++) { mouse_cmd(0xF3); mouse_cmd(knock[i]); }
    mouse_cmd(0xF2);            /* get device ID (after the ACK) */
    int id = ps2_read_data();
    if (id == 3 || id == 4) packet_len = 4;
    mouse_cmd(0xF3);            /* set sample rate ... */
    mouse_cmd(100);             /* ... 100 samples/s */
    mouse_cmd(0xF4);            /* enable data reporting */
    irq_register(12, mouse_irq);
    kprintf("[mous] PS/2 mouse ready (%s)\n", packet_len == 4 ? "wheel" : "3 buttons");
}

void mouse_set_speed(int quarters)
{
    if (quarters < 1) quarters = 1;
    if (quarters > 12) quarters = 12;
    speed = quarters;
}

void mouse_set_bounds(int w, int h)
{
    u32 f = irq_save();
    maxw = w; maxh = h;
    mx = w / 2; my = h / 2;
    irq_restore(f);
}

bool mouse_poll(struct mouse_event *ev)
{
    u32 f = irq_save();
    bool ok = qtail != qhead;
    if (ok) { *ev = queue[qtail]; qtail = (qtail + 1) % QSIZE; }
    irq_restore(f);
    return ok;
}

void mouse_get(int *x, int *y, u8 *b)
{
    u32 f = irq_save();
    *x = mx; *y = my; *b = buttons;
    irq_restore(f);
}
