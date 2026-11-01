/* NimbusOS - Raspberry Pi framebuffer from the VideoCore firmware
 * (mailbox property tags: physical/virtual size, depth, pixel order,
 * allocate buffer, get pitch).  Pixels are 0x00RRGGBB like the PC build,
 * which is the firmware's "BGR" byte order.  The buffer lives in the GPU's
 * memory, which the MMU maps uncached at its own address. */
#include "arch/arm64/pi.h"
#include "drivers/fb.h"
#include "drivers/vga_text.h"
#include "kernel/kprintf.h"

struct framebuffer g_fb;
static volatile u32 mb[36] __attribute__((aligned(16)));
static u32 fb_bytes;

u32 fb_vram_bytes(void) { return 64u * 1024 * 1024; }

bool fb_mode_supported(int w, int h)
{
    return w >= 320 && h >= 180 && w <= 1920 && h <= 1200;
}

static bool display_size(int *w, int *h)
{
    int i = 0;
    mb[i++] = 0; mb[i++] = 0;
    mb[i++] = 0x00040003; mb[i++] = 8; mb[i++] = 0; mb[i++] = 0; mb[i++] = 0;
    mb[i++] = 0;
    mb[0] = i * 4;
    if (!mbox_call(mb)) return false;
    *w = (int)mb[5]; *h = (int)mb[6];
    return *w > 0 && *h > 0;
}

static bool set_mode(int w, int h)
{
    if (g_fb.pixels) {                                   /* give the old buffer back */
        int i = 0;
        mb[i++] = 0; mb[i++] = 0;
        mb[i++] = 0x00048001; mb[i++] = 0; mb[i++] = 0;
        mb[i++] = 0;
        mb[0] = i * 4;
        mbox_call(mb);
    }
    int i = 0;
    mb[i++] = 0; mb[i++] = 0;
    mb[i++] = 0x00048003; mb[i++] = 8; mb[i++] = 0; mb[i++] = (u32)w; mb[i++] = (u32)h;   /* physical size */
    mb[i++] = 0x00048004; mb[i++] = 8; mb[i++] = 0; mb[i++] = (u32)w; mb[i++] = (u32)h;   /* virtual size */
    mb[i++] = 0x00048009; mb[i++] = 8; mb[i++] = 0; mb[i++] = 0; mb[i++] = 0;             /* offset */
    mb[i++] = 0x00048005; mb[i++] = 4; mb[i++] = 0; mb[i++] = 32;                         /* depth */
    mb[i++] = 0x00048006; mb[i++] = 4; mb[i++] = 0; mb[i++] = 0;                          /* pixel order: BGR */
    int alpha = i;
    mb[i++] = 0x00048007; mb[i++] = 4; mb[i++] = 0; mb[i++] = 2;                          /* alpha: ignored */
    int alloc = i;
    mb[i++] = 0x00040001; mb[i++] = 8; mb[i++] = 0; mb[i++] = 4096; mb[i++] = 0;          /* allocate */
    int pitch = i;
    mb[i++] = 0x00040008; mb[i++] = 4; mb[i++] = 0; mb[i++] = 0;                          /* pitch */
    mb[i++] = 0;
    mb[0] = i * 4;
    if (!mbox_call(mb) || !mb[alloc + 3]) return false;
    int gw = (int)mb[5], gh = (int)mb[6];
    u32 base = mb[alloc + 3] & 0x3FFFFFFF;
    g_fb.phys = base;
    g_fb.pixels = (u32 *)(uptr)base;
    g_fb.width = gw;
    g_fb.height = gh;
    g_fb.pitch = (int)(mb[pitch + 3] / 4);
    fb_bytes = mb[alloc + 4];
    if (mb[24] != 0) klog("[fb  ] firmware chose RGB pixel order; colours will be swapped\n");
    /* The Pi 4 firmware otherwise treats the top byte as alpha, and 0 is
     * fully transparent: the picture would be invisible. */
    klog("[fb  ] alpha mode %u (2 = ignored)\n", mb[alpha + 3]);
    return gw > 0 && gh > 0;
}

/* Called before anything else needs the screen: picks the monitor's own
 * resolution (up to 1920x1200), or 1024x768 when it reports nothing useful. */
bool fb_early(void)
{
    int w = 1024, h = 768, dw, dh;
    bool known = display_size(&dw, &dh);
    klog("[fb  ] firmware reports the display as %s %dx%d\n", known ? "" : "(failed)", known ? dw : 0, known ? dh : 0);
    if (known && dw >= 800 && dh >= 600) { w = dw; h = dh; }
    if (w > 1920) { w = 1920; h = 1080; }
    if (!set_mode(w, h) && !set_mode(1024, 768)) return false;
    return true;
}

bool fb_set_mode(int w, int h)
{
    if (!fb_mode_supported(w, h) || !set_mode(w, h)) return false;
    kprintf("[fb  ] mode %dx%dx32\n", g_fb.width, g_fb.height);
    return true;
}

bool fb_init(int w, int h)
{
    if (!g_fb.pixels && !fb_early()) return false;
    vga_disable();
    kprintf("[fb  ] %dx%dx32 framebuffer at %08x (%u KiB, pitch %d)\n",
            g_fb.width, g_fb.height, g_fb.phys, fb_bytes / 1024, g_fb.pitch);
    return true;
}
