#ifndef NIMBUS_VMM_H
#define NIMBUS_VMM_H
#include "kernel/types.h"

/*
 * Virtual memory map (see README "Memory layout"), x86-64 4-level paging:
 *   0x00000000-0x00000FFF  unmapped (NULL guard)
 *   0x00001000-RAM top     identity mapped (all RAM up to 3 GiB): BIOS area,
 *                          boot data, kernel, page tables, RAM disk
 *   0xC0000000-0xCFFFFFFF  kernel heap (grown on demand)
 *   0xD0000000-0xD0FFFFFF  DMA buffers for devices (uncached)
 *   0xD1000000-0xD1FFFFFF  device MMIO registers (uncached)
 *   0xE0000000-0xEFFFFFFF  linear framebuffer
 *   0x8000000000-...       user programs (per-process address spaces)
 * Other physical ranges (ACPI tables, MMIO) are identity mapped on demand.
 */
#define VMM_IDENTITY_END 0x01000000   /* always identity mapped */
#define VMM_HEAP_BASE    0xC0000000
#define VMM_HEAP_MAX     0xD0000000
#define VMM_DMA_BASE     0xD0000000
#define VMM_DMA_END      0xD1000000
#define VMM_MMIO_BASE    0xD1000000   /* device registers (e1000 BAR0) */
#define VMM_FB_BASE      0xE0000000
#define USER_BASE        0x0000008000000000ull   /* PML4 entry 1 */
#define USER_STACK_TOP   0x0000008080000000ull

#define PTE_PRESENT  0x001
#define PTE_WRITE    0x002
#define PTE_USER     0x004
#define PTE_PWT      0x008
#define PTE_PCD      0x010   /* cache disable */

void vmm_init(u64 ram_top);
bool vmm_map(uptr virt, u64 phys, u32 flags);
u64  vmm_unmap(uptr virt);                /* returns the physical frame */
u64  vmm_translate(uptr virt);            /* 0 if not mapped */
void vmm_map_range(uptr virt, u64 phys, u64 size, u32 flags);
void vmm_identity_map(u64 phys, u64 size);

/* User address spaces: a space is the physical address of its PML4. */
u64  vmm_space_create(void);
bool vmm_space_map(u64 space, uptr virt, u64 phys, u32 flags);
u64  vmm_space_translate(u64 space, uptr virt);
void vmm_space_destroy(u64 space);
void vmm_space_switch(u64 space);          /* 0 = kernel-only space */
u64  vmm_space_current(void);
#endif
