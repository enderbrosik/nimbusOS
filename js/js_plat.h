/* NimbusOS JavaScript engine - platform layer.
 * The engine uses only memcpy/memmove/memset/memcmp/strlen and a heap, so the same sources build into the kernel
 * (no libc, no floating point) and on the host for testing. */
#ifndef JS_PLAT_H
#define JS_PLAT_H
#ifdef NIMBUS_KERNEL
#include "kernel/types.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#define JS_MALLOC(n)     kmalloc(n)
#define JS_REALLOC(p, n) krealloc((p), (n))
#define JS_FREE(p)       kfree(p)
#else
#include <stdlib.h>
#include <string.h>
#define JS_MALLOC(n)     malloc(n)
#define JS_REALLOC(p, n) realloc((p), (n))
#define JS_FREE(p)       free(p)
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#endif
