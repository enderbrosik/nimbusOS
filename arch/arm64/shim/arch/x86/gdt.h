/* NimbusOS - Raspberry Pi stand-in for arch/x86/gdt.h (no segments on ARM). */
#ifndef NIMBUS_GDT_H
#define NIMBUS_GDT_H
#include "kernel/types.h"
static inline void tss_set_kernel_stack(uptr sp) { (void)sp; }
#endif
