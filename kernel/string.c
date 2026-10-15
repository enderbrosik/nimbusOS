/* NimbusOS - freestanding string / memory routines. */
#include "kernel/string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    u32 d0, d1, d2;
    /* Bulk copy with rep movsd, then the tail with rep movsb. */
    __asm__ volatile("rep movsl\n\t"
                     "movl %4, %%ecx\n\t"
                     "rep movsb"
                     : "=&c"(d0), "=&D"(d1), "=&S"(d2)
                     : "0"(n >> 2), "g"(n & 3), "1"(dst), "2"(src)
                     : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    /* Overlapping, copy backwards. */
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    u32 d0, d1;
    __asm__ volatile("rep stosb" : "=&c"(d0), "=&D"(d1) : "0"(n), "1"(dst), "a"(c) : "memory");
    return ret;
}

void memset32(u32 *dst, u32 v, size_t count)
{
    u32 d0, d1;
    __asm__ volatile("rep stosl" : "=&c"(d0), "=&D"(d1) : "0"(count), "1"(dst), "a"(v) : "memory");
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (u8)*a - (u8)*b;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((u8)*a) - tolower((u8)*b);
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return c == 0 ? (char *)s : (char *)r;
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, nl)) return (char *)h;
    return NULL;
}

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = 10;
    for (;;) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
