/*
 * NimbusOS - linear framebuffer via the Bochs/QEMU "DISPI" VBE extensions
 * (QEMU -vga std, PCI device 1234:1111).  The mode is programmed through
 * index/data ports 0x1CE/0x1CF; the framebuffer's physical address is PCI
 * BAR0.  Reference: OSDev wiki "Bochs VBE Extensions", QEMU docs/specs/
 * standard-vga.rst.
 */
#include "drivers/fb.h"
#include "drivers/pci.h"
#include "arch/x86/io.h"
#include "kernel/vmm.h"
#include "kernel/kprintf.h"
#include "drivers/vga_text.h"

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF
enum { DISPI_ID, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE, DISPI_BANK,
       DISPI_VIRT_W, DISPI_VIRT_H, DISPI_X_OFF, DISPI_Y_OFF, DISPI_VIDEO_MEM_64K };
#define DISPI_ENABLED 0x01
#define DISPI_LFB     0x40

struct framebuffer g_fb;

static void dispi_write(u16 idx, u16 v) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, v); }
static u16 dispi_read(u16 idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

static u32 vram;

u32 fb_vram_bytes(void) { return vram; }

bool fb_mode_supported(int w, int h)
{
    return w >= 640 && h >= 480 && w <= FB_MAX_W && h <= FB_MAX_H && (u32)w * h * 4 <= vram;
}

bool fb_set_mode(int w, int h)
{
    if (!fb_mode_supported(w, h)) return false;
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, w);
    dispi_write(DISPI_YRES, h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_W, w);
    dispi_write(DISPI_X_OFF, 0);
    dispi_write(DISPI_Y_OFF, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h) return false;
    g_fb.width = w;
    g_fb.height = h;
    g_fb.pitch = w;
    kprintf("[fb  ] mode %dx%dx32\n", w, h);
    return true;
}

bool fb_init(int w, int h)
{
    u16 id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF) {
        kprintf("[fb  ] Bochs VBE not present (id %04x)\n", id);
        return false;
    }
    u32 phys = 0xFD000000;              /* QEMU default if PCI lookup fails */
    u8 bus, dev, fn;
    if (pci_find(0x1234, 0x1111, &bus, &dev, &fn))
        phys = pci_read32(bus, dev, fn, 0x10) & 0xFFFFFFF0;
    /* index 0x0A reports video memory in 64 KiB units (QEMU / Bochs >= 0xB0C5) */
    vram = id >= 0xB0C5 ? (u32)dispi_read(DISPI_VIDEO_MEM_64K) * 65536 : 0;
    if (!vram) vram = 8 * 1024 * 1024;
    /* map enough for the largest mode once, so modes can change later */
    u32 map = MIN(vram, (u32)FB_MAX_W * FB_MAX_H * 4);
    vmm_map_range(VMM_FB_BASE, phys, map, PTE_WRITE);
    g_fb.pixels = (u32 *)VMM_FB_BASE;
    g_fb.phys = phys;
    if (!fb_set_mode(w, h) && !fb_set_mode(1024, 768)) return false;
    vga_disable();
    kprintf("[fb  ] linear framebuffer phys %08x -> virt %08x, %u KiB VRAM (id %04x)\n",
            phys, VMM_FB_BASE, vram / 1024, id);
    kprintf("[fb  ] %dx%dx32 linear framebuffer\n", g_fb.width, g_fb.height);
    return true;
}
