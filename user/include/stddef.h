/* NimbusOS libc - <stddef.h> */
#ifndef _STDDEF_H
#define _STDDEF_H
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef long             ssize_t;
#define NULL ((void *)0)
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
