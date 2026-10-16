#ifndef NIMBUS_GDT_H
#define NIMBUS_GDT_H
#include "kernel/types.h"
/* Selector layout.  User data precedes user code so that SYSRET-style
 * ordering (STAR) would also work; user selectors carry RPL 3. */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   (0x18 | 3)
#define USER_CS   (0x20 | 3)
#define TSS_SEL   0x28
void gdt_init(void);
/* Kernel stack the CPU switches to on an interrupt from ring 3. */
void tss_set_kernel_stack(uptr rsp0);
#endif
