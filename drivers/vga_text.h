#ifndef NIMBUS_VGA_TEXT_H
#define NIMBUS_VGA_TEXT_H
#include "kernel/types.h"
enum { VGA_BLACK, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LGREY,
       VGA_DGREY, VGA_LBLUE, VGA_LGREEN, VGA_LCYAN, VGA_LRED, VGA_LMAGENTA, VGA_YELLOW, VGA_WHITE };
void vga_init(void);
void vga_clear(void);
void vga_setcolor(u8 fg, u8 bg);
void vga_putc(char c);
void vga_write(const char *s);
void vga_disable(void);      /* stop writing (graphics mode active) */
#endif
