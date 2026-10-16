/* NimbusOS libc - <stdlib.h> */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);
__attribute__((noreturn)) void exit(int code);
__attribute__((noreturn)) void abort(void);
int   atoi(const char *s);
long  strtol(const char *s, char **end, int base);
int   abs(int x);
long  labs(long x);
int   rand(void);
void  srand(unsigned seed);
void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
#endif
