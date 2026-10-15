/*
 * NimbusOS - PCI configuration space access, mechanism #1
 * (ports 0xCF8 address / 0xCFC data). OSDev wiki "PCI".
 */
#include "drivers/pci.h"
#include "arch/x86/io.h"
#include "kernel/kprintf.h"

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

bool pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn)
{
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                u32 id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                    *bus = b; *dev = d; *fn = f;
                    return true;
                }
            }
    return false;
}

void pci_enumerate(void)
{
    for (int d = 0; d < 32; d++)
        for (int f = 0; f < 8; f++) {
            u32 id = pci_read32(0, d, f, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
            u32 cls = pci_read32(0, d, f, 8);
            klog("[pci ] 00:%02x.%u %04x:%04x class %02x.%02x\n", d, f, id & 0xFFFF, id >> 16,
                 cls >> 24, (cls >> 16) & 0xFF);
        }
}
