/* NimbusOS - boot console on the Pi framebuffer (the vga_text.h interface).
 * Shows every log line from power-on until the desktop takes the screen.
 * Text wraps from the bottom back to the top instead of scrolling, because
 * reading the uncached framebuffer back would be slow. */
#include "arch/arm64/pi.h"
#include "drivers/vga_text.h"
#include "drivers/fb.h"
#include "kernel/bootinfo.h"

extern const unsigned char pi_vga_font[4096];
static const u32 palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xC0C0C0,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
static int col, row, cols, rows;
static u32 fg = 0xC0C0C0, bg = 0x000000;
static bool active;

static void clear_row(int r)
{
    if (r >= rows) return;
    for (int y = r * 16; y < r * 16 + 16; y++) {
        u32 *p = g_fb.pixels + (u64)y * g_fb.pitch;
        for (int x = 0; x < cols * 8; x++) p[x] = bg;
    }
}

void vga_init(void)
{
    if (!g_fb.pixels) return;
    cols = g_fb.width / 8;
    rows = g_fb.height / 16;
    active = true;
    vga_clear();
}

void vga_clear(void)
{
    if (!active) return;
    for (int y = 0; y < g_fb.height; y++) {
        u32 *p = g_fb.pixels + (u64)y * g_fb.pitch;
        for (int x = 0; x < g_fb.width; x++) p[x] = bg;
    }
    col = row = 0;
}

void vga_setcolor(u8 f, u8 b) { fg = palette[f & 15]; bg = palette[b & 15]; }
void vga_disable(void) { active = false; }
void console_disable(void) { active = false; }

static void newline(void)
{
    col = 0;
    if (++row >= rows) row = 0;
    clear_row(row);
    clear_row(row + 1 < rows ? row + 1 : 0);     /* a blank line marks where the text is */
}

void vga_putc(char c)
{
    if (!active) return;
    if (c == '\n') { newline(); return; }
    if (c == '\r') { col = 0; return; }
    if (col >= cols) newline();
    const unsigned char *g = pi_vga_font + (u8)c * 16;
    for (int y = 0; y < 16; y++) {
        u32 *p = g_fb.pixels + (u64)(row * 16 + y) * g_fb.pitch + col * 8;
        u8 bits = g[y];
        for (int x = 0; x < 8; x++) p[x] = (bits & (0x80 >> x)) ? fg : bg;
    }
    col++;
}

/* Shared code calls vga_write next to serial_write; on the Pi every serial
 * line already reaches the screen through console_write, so this is empty. */
void vga_write(const char *s) { (void)s; }
void console_write(const char *s) { while (*s) vga_putc(*s++); }
