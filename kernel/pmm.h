#ifndef NIMBUS_PMM_H
#define NIMBUS_PMM_H
#include "kernel/types.h"
#define PAGE_SIZE 4096
void pmm_init(void);
u32  pmm_alloc(void);           /* returns physical address of a free 4 KiB frame, 0 if none */
void pmm_free(u32 phys);
u32  pmm_total_frames(void);
u64  pmm_ram_top(void);          /* end of the highest usable RAM below 3 GiB */
u32  pmm_free_frames(void);
#endif
