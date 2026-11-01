/* NimbusOS - PL011 UART0 on GPIO 14/15 (115200 8N1) as the serial log.
 * Every line also goes to the on-screen boot console and the SD card log. */
#include "arch/arm64/pi.h"
#include "drivers/serial.h"
#include "drivers/vga_text.h"

#define UART   (g_periph + 0x201000)
#define DR     0x00
#define FR     0x18
#define IBRD   0x24
#define FBRD   0x28
#define LCRH   0x2C
#define CR     0x30
#define ICR    0x44
#define GPFSEL1 0x200004

static bool ready;

void uart_init(void)
{
    mmio_w(UART + CR, 0);
    u32 sel = mmio_r(g_periph + GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (4u << 12) | (4u << 15);                  /* ALT0: TXD0 / RXD0 */
    mmio_w(g_periph + GPFSEL1, sel);
    mmio_w(UART + ICR, 0x7FF);
    mmio_w(UART + IBRD, 26);                         /* 48 MHz / (16 * 115200) = 26.04 */
    mmio_w(UART + FBRD, 3);
    mmio_w(UART + LCRH, (3 << 5) | (1 << 4));        /* 8 bits, FIFOs on */
    mmio_w(UART + CR, (1 << 0) | (1 << 8) | (1 << 9));
    ready = true;
}

void serial_init(void) {}

void serial_putc(char c)
{
    if (!ready) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && (mmio_r(UART + FR) & (1 << 5)); i++) {}
    mmio_w(UART + DR, (u8)c);
}

void serial_write(const char *s)
{
    sdlog_append(s);
    console_write(s);
    for (; *s; s++) serial_putc(*s);
}
