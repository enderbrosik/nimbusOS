/*
 * NimbusOS - icons, drawn procedurally at boot with the gfx primitives into
 * 32x32 ARGB bitmaps (no image assets needed).  Each icon is painted on a
 * magenta "key" background that becomes transparent; 16x16 versions are
 * produced by a 2x2 box filter with alpha.
 */
#include "gui/icons.h"
#include "kernel/heap.h"
#include "kernel/string.h"

#define KEY 0xFF00FF
static u32 big[ICON_COUNT][32 * 32];
static u32 small[ICON_COUNT][16 * 16];

static void page(surface_t *s, u32 fill)
{
    /* sheet of paper with a folded top-right corner */
    gfx_fill_rect(s, 7, 2, 15, 28, fill);
    gfx_fill_rect(s, 22, 9, 4, 21, fill);
    for (int i = 0; i < 7; i++) gfx_hline(s, 22, 2 + i, i + 1, fill);
    gfx_line(s, 7, 2, 21, 2, RGB(120, 120, 130));
    gfx_line(s, 7, 2, 7, 29, RGB(120, 120, 130));
    gfx_line(s, 7, 29, 25, 29, RGB(120, 120, 130));
    gfx_line(s, 25, 9, 25, 29, RGB(120, 120, 130));
    gfx_line(s, 21, 2, 25, 6, RGB(120, 120, 130));
    gfx_line(s, 25, 6, 25, 9, RGB(120, 120, 130));
    gfx_fill_rect(s, 21, 3, 1, 6, RGB(200, 200, 205));
    gfx_hline(s, 21, 8, 4, RGB(200, 200, 205));
}

static void folder(surface_t *s, u32 body, u32 dark)
{
    gfx_round_rect(s, 2, 5, 13, 6, 2, dark);
    gfx_fill_rect(s, 2, 8, 28, 20, dark);
    gfx_fill_rect(s, 3, 9, 26, 18, color_lighten(body, 30));
    gfx_gradient_v(s, 2, 12, 28, 16, body, color_darken(body, 30));
    gfx_hline(s, 2, 12, 28, color_lighten(body, 90));
    gfx_rect(s, 2, 12, 28, 16, dark);
}

static void draw_icon(surface_t *s, int id)
{
    gfx_fill_rect(s, 0, 0, 32, 32, KEY);
    switch (id) {
    case ICON_FOLDER:
        folder(s, RGB(250, 200, 70), RGB(196, 140, 30));
        break;
    case ICON_EXPLORER:
        folder(s, RGB(250, 200, 70), RGB(196, 140, 30));
        gfx_fill_circle(s, 20, 20, 6, RGB(220, 240, 255));
        gfx_circle(s, 20, 20, 6, RGB(30, 80, 160));
        gfx_circle(s, 20, 20, 5, RGB(30, 80, 160));
        gfx_thick_line(s, 24, 24, 29, 29, 1, RGB(30, 80, 160));
        break;
    case ICON_FILE:
        page(s, 0xFFFFFF);
        break;
    case ICON_FILE_TEXT:
        page(s, 0xFFFFFF);
        for (int y = 11; y < 27; y += 3) gfx_hline(s, 10, y, y == 11 ? 9 : 12, RGB(110, 130, 170));
        break;
    case ICON_FILE_IMAGE:
    case ICON_WALLPAPER:
        page(s, 0xFFFFFF);
        gfx_fill_rect(s, 9, 11, 14, 15, RGB(120, 190, 250));
        gfx_fill_circle(s, 18, 15, 2, RGB(255, 220, 60));
        for (int x = 9; x < 23; x++) {
            int h = 4 + ((x * 7) % 5);
            gfx_vline(s, x, 26 - h, h, RGB(60, 160, 70));
        }
        break;
    case ICON_NOTEPAD:
        gfx_fill_rect(s, 5, 4, 22, 26, RGB(255, 255, 240));
        gfx_rect(s, 5, 4, 22, 26, RGB(120, 120, 120));
        gfx_fill_rect(s, 5, 4, 22, 5, RGB(60, 110, 190));
        for (int x = 8; x < 26; x += 4) { gfx_vline(s, x, 2, 5, RGB(170, 170, 180)); gfx_pixel(s, x, 2, RGB(90, 90, 90)); }
        for (int y = 13; y < 28; y += 3) gfx_hline(s, 8, y, 16, RGB(150, 170, 200));
        gfx_thick_line(s, 13, 26, 24, 13, 1, RGB(230, 160, 40));
        gfx_line(s, 12, 27, 13, 26, RGB(40, 40, 40));
        break;
    case ICON_PAINT:
        gfx_fill_circle(s, 15, 17, 13, RGB(150, 110, 70));
        gfx_fill_circle(s, 15, 17, 12, RGB(235, 205, 160));
        gfx_fill_circle(s, 19, 23, 3, KEY);
        gfx_fill_circle(s, 9, 12, 3, RGB(220, 40, 40));
        gfx_fill_circle(s, 16, 9, 3, RGB(40, 120, 230));
        gfx_fill_circle(s, 23, 13, 3, RGB(40, 180, 60));
        gfx_fill_circle(s, 8, 20, 3, RGB(250, 210, 30));
        gfx_thick_line(s, 30, 2, 20, 16, 1, RGB(110, 70, 30));
        gfx_fill_circle(s, 20, 16, 2, RGB(60, 60, 60));
        break;
    case ICON_TERMINAL:
        gfx_round_rect(s, 2, 4, 28, 24, 3, RGB(60, 64, 72));
        gfx_fill_rect(s, 4, 9, 24, 17, RGB(12, 12, 16));
        gfx_hline(s, 4, 6, 3, RGB(240, 90, 80));
        gfx_line(s, 7, 13, 11, 17, RGB(80, 230, 100));
        gfx_line(s, 11, 17, 7, 21, RGB(80, 230, 100));
        gfx_line(s, 7, 14, 10, 17, RGB(80, 230, 100));
        gfx_line(s, 10, 17, 7, 20, RGB(80, 230, 100));
        gfx_hline(s, 13, 21, 8, RGB(200, 200, 200));
        break;
    case ICON_TASKMGR:
    case ICON_COMPUTER:
        gfx_round_rect(s, 2, 4, 28, 20, 2, RGB(50, 54, 64));
        gfx_fill_rect(s, 4, 6, 24, 16, id == ICON_TASKMGR ? RGB(10, 30, 20) : RGB(40, 110, 200));
        gfx_fill_rect(s, 13, 24, 6, 3, RGB(90, 90, 100));
        gfx_fill_rect(s, 8, 27, 16, 2, RGB(70, 70, 80));
        if (id == ICON_TASKMGR) {
            int pts[] = { 18, 16, 17, 12, 14, 15, 9, 11, 13, 8 };
            for (int i = 0; i < 9; i++) gfx_line(s, 5 + i * 5 / 2, pts[i], 5 + (i + 1) * 5 / 2, pts[i + 1], RGB(80, 255, 120));
        } else {
            gfx_gradient_v(s, 4, 6, 24, 16, RGB(60, 140, 230), RGB(20, 70, 160));
        }
        break;
    case ICON_INSTALLER:
        gfx_round_rect(s, 3, 14, 26, 14, 2, RGB(90, 96, 110));
        gfx_fill_rect(s, 5, 16, 22, 10, RGB(170, 176, 190));
        gfx_fill_circle(s, 23, 23, 1, RGB(60, 220, 80));
        gfx_fill_rect(s, 13, 1, 6, 9, RGB(40, 150, 60));
        for (int i = 0; i < 6; i++) gfx_hline(s, 10 + i, 10 + i, 12 - 2 * i, RGB(40, 150, 60));
        break;
    case ICON_DRIVE:
        gfx_round_rect(s, 2, 10, 28, 14, 2, RGB(90, 96, 110));
        gfx_fill_rect(s, 4, 12, 24, 10, RGB(180, 186, 198));
        gfx_hline(s, 6, 17, 14, RGB(120, 126, 140));
        gfx_fill_circle(s, 24, 17, 1, RGB(60, 220, 80));
        break;
    case ICON_SHUTDOWN:
        gfx_fill_circle(s, 15, 16, 13, RGB(210, 50, 50));
        gfx_circle(s, 15, 16, 7, 0xFFFFFF);
        gfx_circle(s, 15, 16, 6, 0xFFFFFF);
        gfx_fill_rect(s, 12, 5, 7, 8, RGB(210, 50, 50));
        gfx_fill_rect(s, 14, 6, 3, 10, 0xFFFFFF);
        break;
    case ICON_RESTART:
        gfx_fill_circle(s, 15, 16, 13, RGB(40, 150, 70));
        gfx_circle(s, 15, 16, 7, 0xFFFFFF);
        gfx_circle(s, 15, 16, 6, 0xFFFFFF);
        gfx_fill_rect(s, 15, 6, 9, 8, RGB(40, 150, 70));
        for (int i = 0; i < 5; i++) gfx_hline(s, 15 + i, 6 + i, 9 - 2 * i, 0xFFFFFF);
        break;
    case ICON_SETTINGS:
        for (int a = 0; a < 8; a++) {
            static const int dx[8] = { 0, 9, 12, 9, 0, -9, -12, -9 }, dy[8] = { -12, -9, 0, 9, 12, 9, 0, -9 };
            gfx_fill_circle(s, 16 + dx[a], 16 + dy[a], 3, RGB(110, 116, 130));
        }
        gfx_fill_circle(s, 16, 16, 10, RGB(110, 116, 130));
        gfx_fill_circle(s, 16, 16, 4, KEY);
        break;
    case ICON_MINES:
        gfx_fill_circle(s, 15, 17, 10, RGB(30, 30, 36));
        for (int a = 0; a < 4; a++) {
            static const int dx[4] = { 0, 13, 9, 9 }, dy[4] = { 13, 0, 9, -9 };
            gfx_line(s, 15 - dx[a], 17 - dy[a], 15 + dx[a], 17 + dy[a], RGB(30, 30, 36));
        }
        gfx_fill_rect(s, 10, 12, 4, 4, 0xFFFFFF);
        gfx_line(s, 22, 6, 27, 1, RGB(200, 120, 40));
        gfx_fill_circle(s, 27, 2, 1, RGB(255, 200, 40));
        break;
    case ICON_SNAKE:
        gfx_round_rect(s, 1, 1, 30, 30, 4, RGB(20, 40, 24));
        for (int i = 0; i < 5; i++) gfx_fill_rect(s, 5 + i * 4, 20, 4, 4, RGB(90, 220, 90));
        for (int i = 0; i < 3; i++) gfx_fill_rect(s, 21, 16 - i * 4, 4, 4, RGB(90, 220, 90));
        gfx_fill_rect(s, 21, 8, 4, 4, RGB(160, 255, 140));
        gfx_fill_circle(s, 9, 9, 2, RGB(240, 60, 60));
        break;
    case ICON_CARDS:
        gfx_round_rect(s, 3, 6, 18, 24, 2, RGB(90, 90, 100));
        gfx_round_rect(s, 4, 7, 16, 22, 2, 0xFFFFFF);
        gfx_round_rect(s, 11, 2, 18, 24, 2, RGB(90, 90, 100));
        gfx_round_rect(s, 12, 3, 16, 22, 2, 0xFFFFFF);
        gfx_text_scaled(s, 15, 6, "\x03", 2, RGB(210, 30, 40));
        gfx_char(s, 6, 12, 6, RGB(20, 20, 20), COLOR_NONE);
        break;
    case ICON_AUDIO:
        gfx_round_rect(s, 2, 2, 28, 28, 6, RGB(230, 90, 40));
        gfx_fill_rect(s, 18, 6, 3, 15, 0xFFFFFF);
        gfx_line(s, 20, 6, 26, 9, 0xFFFFFF); gfx_line(s, 20, 7, 26, 10, 0xFFFFFF);
        gfx_fill_circle(s, 14, 21, 5, 0xFFFFFF);
        break;
    case ICON_SPEAKER:
    case ICON_MUTED:
        gfx_fill_rect(s, 5, 12, 6, 8, RGB(220, 224, 232));
        for (int i = 0; i < 8; i++) gfx_vline(s, 11 + i, 12 - i, 8 + 2 * i, RGB(220, 224, 232));
        if (id == ICON_SPEAKER) {
            gfx_line(s, 22, 11, 24, 16, RGB(220, 224, 232)); gfx_line(s, 24, 16, 22, 21, RGB(220, 224, 232));
            gfx_line(s, 25, 7, 28, 16, RGB(220, 224, 232)); gfx_line(s, 28, 16, 25, 25, RGB(220, 224, 232));
        } else {
            gfx_thick_line(s, 21, 11, 29, 21, 1, RGB(240, 80, 70));
            gfx_thick_line(s, 29, 11, 21, 21, 1, RGB(240, 80, 70));
        }
        break;
    case ICON_WEB:
        gfx_fill_circle(s, 16, 16, 13, RGB(40, 120, 210));
        gfx_circle(s, 16, 16, 13, RGB(20, 70, 150));
        for (int i = -1; i <= 1; i++) gfx_hline(s, 4, 16 + i * 6, 25, RGB(170, 210, 255));
        gfx_vline(s, 16, 3, 27, RGB(170, 210, 255));
        gfx_circle(s, 16, 16, 6, RGB(170, 210, 255));
        gfx_fill_rect(s, 7, 8, 7, 5, RGB(80, 180, 90));
        gfx_fill_rect(s, 18, 18, 8, 6, RGB(80, 180, 90));
        break;
    case ICON_BLOCKRUN:                     /* a runner square over spikes */
        gfx_round_rect(s, 3, 3, 26, 26, 3, RGB(40, 46, 66));
        gfx_round_rect(s, 7, 6, 12, 12, 3, RGB(90, 230, 170));
        gfx_fill_rect(s, 9, 9, 2, 2, 0x102030);
        gfx_fill_rect(s, 14, 9, 2, 2, 0x102030);
        for (int r = 0; r < 9; r++) {
            int half = 4 * (9 - r) / 9;
            gfx_fill_rect(s, 20 - half, 26 - 9 + r, half * 2 + 1, 1, RGB(230, 70, 80));
        }
        break;
    case ICON_BREAKOUT:                     /* a ball and paddle under three rows of bricks */
        gfx_round_rect(s, 1, 1, 30, 30, 4, RGB(22, 26, 40));
        for (int c = 0; c < 4; c++) {
            gfx_fill_rect(s, 4 + c * 7, 4, 6, 3, RGB(230, 70, 90));
            gfx_fill_rect(s, 4 + c * 7, 9, 6, 3, RGB(240, 210, 80));
            gfx_fill_rect(s, 4 + c * 7, 14, 6, 3, RGB(90, 200, 120));
        }
        gfx_round_rect(s, 6, 24, 20, 4, 2, RGB(200, 210, 230));
        gfx_fill_circle(s, 16, 20, 2, 0xFFFFFF);
        break;
    case ICON_UPDATE:                       /* green circle with a down arrow (download) */
        gfx_fill_circle(s, 16, 16, 13, RGB(46, 160, 80));
        gfx_circle(s, 16, 16, 13, RGB(24, 110, 50));
        gfx_fill_rect(s, 14, 7, 5, 11, 0xFFFFFF);
        for (int i = 0; i < 6; i++) gfx_hline(s, 10 + i, 18 + i, 13 - 2 * i, 0xFFFFFF);
        gfx_fill_rect(s, 9, 25, 15, 2, RGB(220, 245, 225));
        break;
    case ICON_NETWORK:
        gfx_round_rect(s, 3, 3, 14, 11, 2, RGB(50, 54, 64));
        gfx_fill_rect(s, 5, 5, 10, 7, RGB(60, 140, 230));
        gfx_round_rect(s, 15, 15, 14, 11, 2, RGB(50, 54, 64));
        gfx_fill_rect(s, 17, 17, 10, 7, RGB(60, 140, 230));
        gfx_line(s, 10, 14, 10, 20, RGB(90, 90, 100)); gfx_line(s, 10, 20, 15, 20, RGB(90, 90, 100));
        break;
    case ICON_INFO:
    case ICON_WARNING:
    case ICON_ERROR: {
        u32 c = id == ICON_INFO ? RGB(30, 110, 210) : id == ICON_WARNING ? RGB(240, 180, 20) : RGB(210, 40, 40);
        gfx_fill_circle(s, 16, 16, 14, c);
        if (id == ICON_INFO) {
            gfx_fill_rect(s, 14, 13, 4, 11, 0xFFFFFF);
            gfx_fill_rect(s, 14, 7, 4, 4, 0xFFFFFF);
        } else if (id == ICON_WARNING) {
            gfx_fill_rect(s, 14, 6, 4, 13, 0x202020);
            gfx_fill_rect(s, 14, 22, 4, 4, 0x202020);
        } else {
            gfx_thick_line(s, 10, 10, 22, 22, 1, 0xFFFFFF);
            gfx_thick_line(s, 22, 10, 10, 22, 1, 0xFFFFFF);
        }
        break;
    }
    }
}

void icons_init(void)
{
    surface_t *s = surface_create(32, 32);
    for (int id = 0; id < ICON_COUNT; id++) {
        draw_icon(s, id);
        for (int i = 0; i < 32 * 32; i++) {
            u32 p = s->px[i] & 0xFFFFFF;
            big[id][i] = p == KEY ? 0 : (0xFF000000u | p);
        }
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                u32 a = 0, r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    u32 p = big[id][(y * 2 + k / 2) * 32 + x * 2 + k % 2];
                    u32 pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xFF) * pa;
                    g += ((p >> 8) & 0xFF) * pa;
                    b += (p & 0xFF) * pa;
                }
                small[id][y * 16 + x] = a ? (((a / 4) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a)) : 0;
            }
    }
    surface_destroy(s);
}

void icon_draw(surface_t *s, int icon, int x, int y)
{
    if (icon >= 0 && icon < ICON_COUNT) gfx_blit_argb(s, x, y, big[icon], 32, 32);
}

void icon_draw_small(surface_t *s, int icon, int x, int y)
{
    if (icon >= 0 && icon < ICON_COUNT) gfx_blit_argb(s, x, y, small[icon], 16, 16);
}

int icon_for_file(const char *name, bool is_dir)
{
    if (is_dir) return ICON_FOLDER;
    const char *dot = strrchr(name, '.');
    if (dot) {
        if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".cfg") || !strcasecmp(dot, ".log") || !strcasecmp(dot, ".md"))
            return ICON_FILE_TEXT;
        if (!strcasecmp(dot, ".bmp")) return ICON_FILE_IMAGE;
        if (!strcasecmp(dot, ".wav")) return ICON_AUDIO;
        if (!strcasecmp(dot, ".elf")) return ICON_TERMINAL;
    }
    return ICON_FILE;
}
