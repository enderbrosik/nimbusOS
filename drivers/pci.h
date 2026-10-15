#ifndef NIMBUS_PCI_H
#define NIMBUS_PCI_H
#include "kernel/types.h"
u32  pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
bool pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn);
void pci_enumerate(void);
#endif
