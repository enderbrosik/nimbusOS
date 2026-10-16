#ifndef NIMBUS_DMA_H
#define NIMBUS_DMA_H
#include "kernel/types.h"
/* One zeroed 4 KiB page that a device can DMA to: returns its kernel
 * virtual address and stores the physical address in *phys. */
void *dma_alloc_page(u32 *phys);
#endif
