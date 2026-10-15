/*
 * NimbusOS - 16550 UART driver for COM1 (debug log output).
 * Register layout: OSDev wiki "Serial Ports"; National Semiconductor
 * PC16550D datasheet section 8 (register descriptions).
 */
#include "drivers/serial.h"
#include "arch/x86/io.h"

#define COM1 0x3F8

static bool serial_ok;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* IER: disable interrupts            */
    outb(COM1 + 3, 0x80);   /* LCR: DLAB=1 to set baud divisor     */
    outb(COM1 + 0, 0x01);   /* divisor lo: 115200 / 1 = 115200 baud */
    outb(COM1 + 1, 0x00);   /* divisor hi                           */
    outb(COM1 + 3, 0x03);   /* LCR: 8 data bits, no parity, 1 stop  */
    outb(COM1 + 2, 0xC7);   /* FCR: enable+clear FIFOs, 14-byte thr */
    outb(COM1 + 4, 0x1E);   /* MCR: loopback mode for self test     */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { serial_ok = false; return; }
    outb(COM1 + 4, 0x0F);   /* MCR: normal mode, DTR/RTS/OUT1/OUT2  */
    serial_ok = true;
}

void serial_putc(char c)
{
    if (!serial_ok) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) /* LSR.THRE */
        ;
    outb(COM1, (u8)c);
}

void serial_write(const char *s) { while (*s) serial_putc(*s++); }
