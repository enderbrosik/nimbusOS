#ifndef NIMBUS_PIT_H
#define NIMBUS_PIT_H
#include "kernel/types.h"
#define TIMER_HZ 1000
void pit_init(void);
u64  timer_ticks(void);      /* milliseconds since boot (1 tick = 1 ms) */
void timer_busy_wait(u32 ms);
#endif
