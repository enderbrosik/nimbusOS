/*
 * NimbusOS - DMA memory.  Devices (AC'97, e1000) need physical addresses;
 * the kernel needs virtual ones.  Pages come from the PMM and are mapped
 * into a dedicated window (VMM_DMA_BASE) with caching disabled, so CPU
 * writes are visible to bus-master DMA without explicit cache flushes.
 */
#include "kernel/dma.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/string.h"
#include "kernel/panic.h"
#include "arch/x86/io.h"

static u32 next_virt = VMM_DMA_BASE;

void *dma_alloc_page(u32 *phys)
{
    u32 f = irq_save();
    u32 p = pmm_alloc();
    if (!p || next_virt >= VMM_DMA_END) { irq_restore(f); return NULL; }
    u32 v = next_virt;
    next_virt += PAGE_SIZE;
    irq_restore(f);
    if (!vmm_map(v, p, PTE_WRITE | PTE_PCD | PTE_PWT)) return NULL;
    memset((void *)(uptr)v, 0, PAGE_SIZE);
    *phys = p;
    return (void *)(uptr)v;
}
