/*
 * NimbusOS - 8042 PS/2 controller.
 * OSDev wiki "8042 PS/2 Controller": status register bit 0 = output buffer
 * full (data for us), bit 1 = input buffer full (controller busy), bit 5 =
 * data came from the auxiliary (mouse) port.  Configuration byte bit 0/1 =
 * IRQ1/IRQ12 enable, bit 6 = scancode translation to set 1.
 */
#include "drivers/ps2.h"
#include "arch/x86/io.h"
#include "kernel/kprintf.h"

bool ps2_wait_write(void)
{
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 2)) return true;
    return false;
}

bool ps2_wait_read(void)
{
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 1) return true;
    return false;
}

void ps2_write_cmd(u8 cmd) { ps2_wait_write(); outb(PS2_CMD, cmd); }
void ps2_write_data(u8 d) { ps2_wait_write(); outb(PS2_DATA, d); }
int  ps2_read_data(void) { return ps2_wait_read() ? inb(PS2_DATA) : -1; }

void ps2_controller_init(void)
{
    ps2_write_cmd(0xAD);                 /* disable keyboard port */
    ps2_write_cmd(0xA7);                 /* disable mouse port */
    while (inb(PS2_STATUS) & 1) inb(PS2_DATA);   /* flush */
    ps2_write_cmd(0x20);                 /* read config byte */
    int cfg = ps2_read_data();
    if (cfg < 0) cfg = 0x45;
    cfg |= 0x01 | 0x02 | 0x40;           /* IRQ1, IRQ12, translation (set 1) */
    cfg &= ~0x30;                        /* enable both port clocks */
    ps2_write_cmd(0x60);
    ps2_write_data((u8)cfg);
    ps2_write_cmd(0xAE);                 /* enable keyboard port */
    ps2_write_cmd(0xA8);                 /* enable mouse port */
    kprintf("[ps2 ] controller configured (cfg %02x)\n", cfg);
}
