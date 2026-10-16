/* NimbusOS libc - memory allocation, conversions, random numbers, exit. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <nimbus.h>

/* ---- malloc: first-fit free list over memory obtained with sbrk ---------- */

struct hdr {
    size_t size;            /* payload bytes */
    struct hdr *next;       /* next free block (free list only) */
    unsigned magic;
    unsigned free;
};
#define MAGIC 0x4E4D4C43u  /* 'NMLC' */
#define HSZ ((sizeof(struct hdr) + 15) & ~(size_t)15)

static struct hdr *free_list;

void *malloc(size_t n)
{
    if (!n) n = 1;
    n = (n + 15) & ~(size_t)15;
    struct hdr **pp = &free_list;
    for (struct hdr *h = free_list; h; pp = &h->next, h = h->next) {
        if (h->size < n) continue;
        if (h->size >= n + HSZ + 32) {         /* split */
            struct hdr *rest = (struct hdr *)((char *)h + HSZ + n);
            rest->size = h->size - n - HSZ;
            rest->magic = MAGIC;
            rest->free = 1;
            rest->next = h->next;
            h->size = n;
            *pp = rest;
        } else {
            *pp = h->next;
        }
        h->free = 0;
        return (char *)h + HSZ;
    }
    size_t grow = n + HSZ < 65536 ? 65536 : n + HSZ;
    char *mem = sbrk((long)grow);
    if (mem == (char *)-1) return NULL;
    struct hdr *h = (struct hdr *)mem;
    h->size = n;
    h->magic = MAGIC;
    h->free = 0;
    if (grow - n - HSZ > HSZ + 32) {
        struct hdr *rest = (struct hdr *)(mem + HSZ + n);
        rest->size = grow - n - 2 * HSZ;
        rest->magic = MAGIC;
        rest->free = 0;
        free((char *)rest + HSZ);
    } else {
        h->size = grow - HSZ;
    }
    return mem + HSZ;
}

void free(void *p)
{
    if (!p) return;
    struct hdr *h = (struct hdr *)((char *)p - HSZ);
    if (h->magic != MAGIC || h->free) {
        fprintf(stderr, "free(): invalid pointer %p\n", p);
        abort();
    }
    h->free = 1;
    /* address-ordered insert with coalescing of adjacent blocks */
    struct hdr **pp = &free_list;
    while (*pp && *pp < h) pp = &(*pp)->next;
    h->next = *pp;
    *pp = h;
    if (h->next && (char *)h + HSZ + h->size == (char *)h->next) {
        h->size += HSZ + h->next->size;
        h->next = h->next->next;
    }
    if (pp != &free_list) {
        struct hdr *prev = (struct hdr *)((char *)pp - offsetof(struct hdr, next));
        if ((char *)prev + HSZ + prev->size == (char *)h) {
            prev->size += HSZ + h->size;
            prev->next = h->next;
        }
    }
}

void *calloc(size_t count, size_t size)
{
    size_t n = count * size;
    if (size && n / size != count) return NULL;
    void *p = malloc(n);
    return p ? memset(p, 0, n) : NULL;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    struct hdr *h = (struct hdr *)((char *)p - HSZ);
    if (h->size >= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size);
    free(p);
    return q;
}

/* ---- conversions --------------------------------------------------------- */

long strtol(const char *s, char **end, int base)
{
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    long v = 0;
    for (;; s++) {
        int d = isdigit(*s) ? *s - '0' : isalpha(*s) ? (*s | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* ---- random numbers (xorshift32) ---------------------------------------- */

static unsigned rng = 2463534242u;
void srand(unsigned seed) { rng = seed ? seed : 2463534242u; }
int rand(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (int)(rng & RAND_MAX);
}

/* ---- sorting (insertion sort for small n, else shell sort) -------------- */

static void swap(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap)
                swap(b + (j - gap) * size, b + j * size, size);
}

/* ---- process ------------------------------------------------------------- */

void __libc_init(void) { srand((unsigned)ticks_ms() * 2654435761u + (unsigned)getpid()); }

void exit(int code)
{
    fflush(stdout);
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

void abort(void)
{
    fputs("abort() called\n", stderr);
    exit(134);
}
