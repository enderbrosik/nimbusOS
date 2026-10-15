#ifndef NIMBUS_HEAP_H
#define NIMBUS_HEAP_H
#include "kernel/types.h"
void  heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *p, size_t size);
void  kfree(void *p);
size_t kmalloc_usable(void *p);
struct heap_stats { u32 mapped, used, free, blocks, free_blocks, allocs, frees; };
void heap_get_stats(struct heap_stats *s);
bool heap_check(void);    /* walk and validate every block */
#endif
