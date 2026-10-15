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
       DISPI_VIRT_W, DISPI_VIRT_H, DISPI_X_OFF, DISPI_Y_OFF };
#define DISPI_ENABLED 0x01
#define DISPI_LFB     0x40

struct framebuffer g_fb;

static void dispi_write(u16 idx, u16 v) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, v); }
static u16 dispi_read(u16 idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

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

    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, w);
    dispi_write(DISPI_YRES, h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_W, w);
    dispi_write(DISPI_X_OFF, 0);
    dispi_write(DISPI_Y_OFF, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);

    u32 size = (u32)w * h * 4;
    vmm_map_range(VMM_FB_BASE, phys, size, PTE_WRITE);
    g_fb.pixels = (u32 *)VMM_FB_BASE;
    g_fb.phys = phys;
    g_fb.width = w;
    g_fb.height = h;
    g_fb.pitch = w;
    vga_disable();
    kprintf("[fb  ] %dx%dx32 linear framebuffer, phys %08x -> virt %08x (id %04x)\n",
            w, h, phys, VMM_FB_BASE, id);
    return true;
}
