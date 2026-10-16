/* NimbusOS libc - string functions. */
#include <string.h>
#include <stdlib.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) { char *d = dst; while ((*d++ = *src++)) {} return dst; }

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) { strcpy(dst + strlen(dst), src); return dst; }

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t len = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, len)) return (char *)h;
    return len ? NULL : (char *)h;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    return d ? memcpy(d, s, n) : NULL;
}

char *strtok(char *s, const char *delim)
{
    static char *next;
    if (s) next = s;
    if (!next) return NULL;
    while (*next && strchr(delim, *next)) next++;
    if (!*next) { next = NULL; return NULL; }
    char *tok = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) *next++ = 0; else next = NULL;
    return tok;
}

const char *strerror(int err)
{
    switch (err) {
    case 0: return "Success";
    case -2: return "No such file or directory";
    case -5: return "I/O error";
    case -12: return "Out of memory";
    case -14: return "Bad address";
    case -17: return "File exists";
    case -20: return "Not a directory";
    case -21: return "Is a directory";
    case -22: return "Invalid argument";
    case -28: return "No space left on device";
    case -36: return "File name too long";
    case -38: return "Function not implemented";
    case -39: return "Directory not empty";
    default: return "Error";
    }
}
