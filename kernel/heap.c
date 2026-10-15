/*
 * NimbusOS - kernel heap (kmalloc/kfree).
 *
 * The heap is a contiguous virtual region starting at VMM_HEAP_BASE that is
 * grown page by page from the PMM.  It is carved into blocks, each with a
 * 16-byte header; blocks are physically adjacent so neighbours can be found
 * from the header (size) and a back pointer (prev).  Allocation is first-fit
 * with splitting; free coalesces with both neighbours.  A magic value catches
 * double frees and heap corruption.  All operations run with interrupts
 * disabled, which is sufficient on a uniprocessor.
 */
#include "kernel/heap.h"
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "arch/x86/io.h"

#define MAGIC_USED 0xA110CA7E
#define MAGIC_FREE 0xF4EEB10C
#define ALIGN 16
#define MIN_SPLIT 32

struct block {
    u32 magic;
    u32 size;               /* payload bytes */
    struct block *prev;     /* physically previous block, NULL for first */
    u32 pad;
};

static struct block *first, *last;
static u32 heap_end;        /* first unmapped address */
static u32 stat_allocs, stat_frees;
static struct block *rover;  /* next-fit start hint */

static inline struct block *next_of(struct block *b)
{
    return b == last ? NULL : (struct block *)((u8 *)(b + 1) + b->size);
}

static bool grow(u32 bytes)
{
    u32 need = ALIGN_UP(bytes, 4096);
    if (heap_end + need > VMM_HEAP_MAX) return false;
    for (u32 a = 0; a < need; a += 4096) {
        u32 frame = pmm_alloc();
        if (!frame || !vmm_map(heap_end + a, frame, PTE_WRITE)) return false;
    }
    u32 old_end = heap_end;
    heap_end += need;
    if (last && last->magic == MAGIC_FREE) {
        last->size += need;
    } else {
        struct block *b = (struct block *)old_end;
        b->magic = MAGIC_FREE;
        b->size = need - sizeof(struct block);
        b->prev = last;
        if (!first) first = b;
        last = b;
    }
    return true;
}

void heap_init(void)
{
    heap_end = VMM_HEAP_BASE;
    first = last = NULL;
    if (!grow(64 * 1024)) panic("heap_init: cannot map initial heap");
    rover = first;
    kprintf("[heap] kernel heap at %p\n", (void *)VMM_HEAP_BASE);
}

static void split(struct block *b, u32 size)
{
    if (b->size < size + sizeof(struct block) + MIN_SPLIT) return;
    struct block *n = (struct block *)((u8 *)(b + 1) + size);
    n->magic = MAGIC_FREE;
    n->size = b->size - size - sizeof(struct block);
    n->prev = b;
    struct block *nn = next_of(b);
    if (nn) nn->prev = n; else last = n;
    b->size = size;
}

void *kmalloc(size_t size)
{
    if (size == 0) size = 1;
    size = ALIGN_UP(size, ALIGN);
    u32 f = irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
        /* next-fit: from rover to end, then from start to rover */
        struct block *b = rover ? rover : first;
        struct block *start = b;
        do {
            if (b->magic == MAGIC_FREE && b->size >= size) {
                split(b, size);
                b->magic = MAGIC_USED;
                rover = next_of(b) ? next_of(b) : first;
                stat_allocs++;
                irq_restore(f);
                return b + 1;
            }
            if (b->magic != MAGIC_FREE && b->magic != MAGIC_USED)
                panic("heap corruption at %p (magic %08x)", b, b->magic);
            b = next_of(b);
            if (!b) b = first;
        } while (b != start);
        if (!grow(size + sizeof(struct block))) break;
    }
    irq_restore(f);
    klog("[heap] out of memory allocating %u bytes\n", (u32)size);
    return NULL;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    u32 f = irq_save();
    struct block *b = (struct block *)p - 1;
    if (b->magic != MAGIC_USED) panic("kfree: bad pointer %p (magic %08x)", p, b->magic);
    b->magic = MAGIC_FREE;
    stat_frees++;
    struct block *n = next_of(b);
    if (n && n->magic == MAGIC_FREE) {            /* merge with next */
        b->size += sizeof(struct block) + n->size;
        struct block *nn = next_of(n);
        if (n == last) last = b; else nn->prev = b;
        if (rover == n) rover = b;
        n->magic = 0;
    }
    if (b->prev && b->prev->magic == MAGIC_FREE) { /* merge into previous */
        struct block *pb = b->prev;
        pb->size += sizeof(struct block) + b->size;
        if (b == last) last = pb; else next_of(pb)->prev = pb;
        if (rover == b) rover = pb;
        b->magic = 0;
    }
    irq_restore(f);
}

size_t kmalloc_usable(void *p) { return p ? ((struct block *)p - 1)->size : 0; }

void *krealloc(void *p, size_t size)
{
    if (!p) return kmalloc(size);
    if (!size) { kfree(p); return NULL; }
    size_t old = kmalloc_usable(p);
    if (old >= size) return p;
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, old);
    kfree(p);
    return n;
}

void heap_get_stats(struct heap_stats *s)
{
    u32 f = irq_save();
    memset(s, 0, sizeof *s);
    s->mapped = heap_end - VMM_HEAP_BASE;
    for (struct block *b = first; b; b = next_of(b)) {
        s->blocks++;
        if (b->magic == MAGIC_FREE) { s->free += b->size; s->free_blocks++; }
        else s->used += b->size;
    }
    s->allocs = stat_allocs;
    s->frees = stat_frees;
    irq_restore(f);
}

bool heap_check(void)
{
    u32 f = irq_save();
    struct block *prev = NULL;
    bool ok = true;
    for (struct block *b = first; b; b = next_of(b)) {
        if ((b->magic != MAGIC_FREE && b->magic != MAGIC_USED) || b->prev != prev ||
            (prev && prev->magic == MAGIC_FREE && b->magic == MAGIC_FREE)) { ok = false; break; }
        prev = b;
    }
    irq_restore(f);
    return ok;
}
