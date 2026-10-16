/* NimbusOS - runtime theme state: screen size and accent colour. */
#include "gui/theme.h"

int g_screen_w = 1024, g_screen_h = 768;
u32 g_accent = ACCENT_DEFAULT;
u32 g_title1 = RGB(0, 84, 166), g_title2 = RGB(38, 128, 214), g_title_border = RGB(0, 60, 120);
u32 g_accent_light = RGB(204, 228, 247);

/* Title bars, selection and focus rings are all derived from one colour. */
void theme_set_accent(u32 c)
{
    g_accent = c;
    g_title1 = color_darken(c, 75);
    g_title2 = color_lighten(c, 30);
    g_title_border = color_darken(c, 125);
    g_accent_light = color_lighten(c, 205);
}
