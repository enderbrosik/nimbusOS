/*
 * NimbusOS - Stage 3 driver demo: exercises the graphics primitives, font,
 * keyboard, mouse and ATA drivers on one screen.  Rendering goes to a back
 * buffer which is then copied to the framebuffer (no flicker).
 */
#include "kernel/drvdemo.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "kernel/bootinfo.h"
#include "kernel/task.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/ata.h"
#include "drivers/rtc.h"
#include "gui/gfx.h"
#include "arch/x86/io.h"
#include "drivers/pit.h"

static const char *arrow[] = {
    "X...........", "XX..........", "X#X.........", "X##X........", "X###X.......", "X####X......",
    "X#####X.....", "X######X....", "X#######X...", "X########X..", "X#####XXXXX.", "X##X##X.....",
    "X#X.X##X....", "XX..X##X....", "X....X##X...", ".....X##X...", "......XX....",
};

static void draw_cursor(surface_t *s, int x, int y)
{
    for (int j = 0; j < 17; j++)
        for (int i = 0; i < 12; i++) {
            char c = arrow[j][i];
            if (c == 'X') gfx_pixel(s, x + i, y + j, 0x000000);
            else if (c == '#') gfx_pixel(s, x + i, y + j, 0xFFFFFF);
        }
}

void drvdemo_run(void)
{
    surface_t fb, *back = surface_create(g_fb.width, g_fb.height);
    surface_wrap(&fb, g_fb.pixels, g_fb.width, g_fb.height, g_fb.pitch);
    char lastkeys[64] = "";
    char status[128] = "";
    int mx = g_fb.width / 2, my = g_fb.height / 2;
    u8 mb = 0;
    int clicks = 0;

    /* ATA check: read MBR of the boot disk, write+read back a scratch sector. */
    char ata_line[160] = "ATA: no drive";
    static u8 sec[512], sec2[512];
    if (g_ata[0].present && ata_read(0, 0, 1, sec) == 0) {
        bool sig = sec[510] == 0x55 && sec[511] == 0xAA;
        u32 dsig = *(u32 *)(sec + 0x1B8);
        for (int i = 0; i < 512; i++) sec[i] = (u8)(i * 7 + 3);
        bool rw = ata_write(0, 4095, 1, sec) == 0 && ata_read(0, 4095, 1, sec2) == 0 && !memcmp(sec, sec2, 512);
        snprintf(ata_line, sizeof ata_line, "ATA: %s, MBR sig %s, disk id %08x (%s), write/readback %s",
                 g_ata[0].model, sig ? "55AA" : "BAD", dsig,
                 dsig == g_bootinfo.disk_signature ? "boot disk" : "other", rw ? "OK" : "FAILED");
        kprintf("[test] %s\n", ata_line);
    }

    for (;;) {
        struct key_event ke;
        while (keyboard_poll(&ke)) {
            if (!ke.pressed) continue;
            if (ke.ch) klog("[test] key %03x ch '%c' mods %02x\n", ke.key, ke.ch, ke.mods);
            else klog("[test] key %03x mods %02x\n", ke.key, ke.mods);
            int n = strlen(lastkeys);
            if (ke.ch == '\b') { if (n) lastkeys[n - 1] = 0; }
            else if (ke.ch >= 32 && n < 60) { lastkeys[n] = ke.ch; lastkeys[n + 1] = 0; }
        }
        struct mouse_event me;
        while (mouse_poll(&me)) {
            if ((me.buttons & 1) && !(mb & 1)) {
                clicks++;
                klog("[test] mouse click at %d,%d\n", me.x, me.y);
            }
            mx = me.x; my = me.y; mb = me.buttons;
        }
        struct rtc_time t;
        rtc_read(&t);
        snprintf(status, sizeof status, "Mouse %4d,%4d buttons %c%c%c clicks %d   RTC %04d-%02d-%02d %02d:%02d:%02d",
                 mx, my, mb & 1 ? 'L' : '-', mb & 4 ? 'M' : '-', mb & 2 ? 'R' : '-', clicks,
                 t.year, t.month, t.day, t.hour, t.minute, t.second);

        gfx_gradient_v(back, 0, 0, back->w, back->h, RGB(16, 60, 110), RGB(10, 20, 40));
        gfx_text_bold(back, 20, 16, "NimbusOS - Stage 3 driver test", 0xFFFFFF);
        gfx_text(back, 20, 40, "Pixels, lines, rectangles, circles, blits and the 8x16 font:", 0xC0D0E0, COLOR_NONE);
        for (int i = 0; i < 64; i++) gfx_pixel(back, 20 + i * 3, 70, 0xFFFF00);
        for (int i = 0; i < 12; i++) gfx_line(back, 20, 90, 20 + i * 20, 200, RGB(255, i * 20, 80));
        gfx_rect(back, 300, 90, 140, 110, 0x00FF00);
        gfx_fill_rect(back, 460, 90, 140, 110, RGB(200, 60, 60));
        gfx_blend_rect(back, 520, 120, 140, 110, RGB(60, 120, 255), 140);
        gfx_circle(back, 760, 145, 55, 0xFFFFFF);
        gfx_fill_circle(back, 900, 145, 50, RGB(255, 180, 0));
        gfx_round_rect(back, 20, 220, 200, 60, 12, RGB(230, 230, 240));
        gfx_text_centered(back, rect_make(20, 220, 200, 60), "Rounded rect", 0x202020);
        /* bitmap blit: copy a region of the back buffer itself */
        surface_t *chip = surface_create(120, 60);
        gfx_gradient_v(chip, 0, 0, 120, 60, 0xFF8080, 0x8080FF);
        gfx_text(chip, 20, 22, "blit()", 0, COLOR_NONE);
        gfx_blit(back, 240, 220, chip, 0, 0, 120, 60);
        surface_destroy(chip);
        for (int c = 0; c < 256; c++)
            gfx_char(back, 20 + (c % 64) * 9, 300 + (c / 64) * 18, (unsigned char)c, 0xFFFFFF, COLOR_NONE);
        gfx_text(back, 20, 390, "Keyboard (type here):", 0xC0D0E0, COLOR_NONE);
        gfx_fill_rect(back, 20, 410, 600, 24, 0xFFFFFF);
        gfx_rect(back, 20, 410, 600, 24, 0x404040);
        gfx_text(back, 26, 414, lastkeys, 0x000000, COLOR_NONE);
        gfx_vline(back, 26 + strlen(lastkeys) * 8, 413, 18, ((u32)timer_ticks() / 500) & 1 ? 0 : 0xFFFFFF);
        gfx_text(back, 20, 450, status, 0xFFFFFF, COLOR_NONE);
        gfx_text(back, 20, 474, ata_line, 0xFFFFFF, COLOR_NONE);
        for (int i = 0; i < ATA_MAX_DRIVES; i++) {
            char l[96];
            if (g_ata[i].present) snprintf(l, sizeof l, "  %-17s %-24s %6u MiB", g_ata[i].name, g_ata[i].model, g_ata[i].sectors / 2048);
            else snprintf(l, sizeof l, "  %-17s (none)", g_ata[i].name);
            gfx_text(back, 20, 498 + i * 18, l, 0xA0C0FF, COLOR_NONE);
        }
        draw_cursor(back, mx, my);
        gfx_blit(&fb, 0, 0, back, 0, 0, back->w, back->h);
        task_sleep(16);
    }
}
