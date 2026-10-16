#ifndef NIMBUS_RANDOM_H
#define NIMBUS_RANDOM_H
#include "kernel/types.h"
u32 rand32(void);                 /* xorshift128, seeded from the TSC and RTC */
u32 rand_range(u32 n);            /* 0 .. n-1 */
#endif
