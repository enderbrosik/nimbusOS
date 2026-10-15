/*
 * NimbusOS - VGA text mode console (80x25, buffer at 0xB8000).
 * Hardware cursor via CRTC registers 0x0E/0x0F (OSDev wiki "Text Mode Cursor").
 */
#include "drivers/vga_text.h"
#include "arch/x86/io.h"

#define VGA_W 80
#define VGA_H 25
static volatile u16 *const vga = (u16 *)0xB8000;
static int cx, cy;
static u8 color = (VGA_BLACK << 4) | VGA_LGREY;
static bool enabled;

static void update_cursor(void)
{
    u16 pos = cy * VGA_W + cx;
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, pos >> 8);
}

void vga_init(void) { enabled = true; vga_clear(); }
void vga_disable(void) { enabled = false; }
void vga_setcolor(u8 fg, u8 bg) { color = (bg << 4) | (fg & 0x0F); }

void vga_clear(void)
{
    if (!enabled) return;
    for (int i = 0; i < VGA_W * VGA_H; i++) vga[i] = (color << 8) | ' ';
    cx = cy = 0;
    update_cursor();
}

static void scroll(void)
{
    for (int i = 0; i < VGA_W * (VGA_H - 1); i++) vga[i] = vga[i + VGA_W];
    for (int i = 0; i < VGA_W; i++) vga[VGA_W * (VGA_H - 1) + i] = (color << 8) | ' ';
    cy = VGA_H - 1;
}

void vga_putc(char c)
{
    if (!enabled) return;
    if (c == '\n') { cx = 0; cy++; }
    else if (c == '\r') cx = 0;
    else if (c == '\t') cx = (cx + 8) & ~7;
    else if (c == '\b') { if (cx > 0) cx--; }
    else { vga[cy * VGA_W + cx] = (color << 8) | (u8)c; cx++; }
    if (cx >= VGA_W) { cx = 0; cy++; }
    if (cy >= VGA_H) scroll();
    update_cursor();
}

void vga_write(const char *s) { while (*s) vga_putc(*s++); }
