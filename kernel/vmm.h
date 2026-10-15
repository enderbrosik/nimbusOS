#ifndef NIMBUS_VMM_H
#define NIMBUS_VMM_H
#include "kernel/types.h"

/*
 * Virtual memory map (see README "Memory layout"):
 *   0x00000000-0x00000FFF  unmapped (NULL guard)
 *   0x00001000-0x00FFFFFF  identity mapped: BIOS area, boot data, kernel
 *   0xC0000000-0xCFFFFFFF  kernel heap (grown on demand)
 *   0xE0000000-0xEFFFFFFF  linear framebuffer
 *   0xFFC00000-0xFFFFFFFF  recursive mapping of the page tables
 * Other physical ranges (ACPI tables, MMIO) are identity mapped on demand.
 */
#define VMM_IDENTITY_END 0x01000000
#define VMM_HEAP_BASE    0xC0000000
#define VMM_HEAP_MAX     0xD0000000
#define VMM_FB_BASE      0xE0000000

#define PTE_PRESENT  0x001
#define PTE_WRITE    0x002
#define PTE_PCD      0x010   /* cache disable */
#define PTE_PWT      0x008

void vmm_init(void);
bool vmm_map(u32 virt, u32 phys, u32 flags);
u32  vmm_unmap(u32 virt);                 /* returns the physical frame */
u32  vmm_translate(u32 virt);             /* 0 if not mapped */
void vmm_map_range(u32 virt, u32 phys, u32 size, u32 flags);
void vmm_identity_map(u32 phys, u32 size);
#endif
