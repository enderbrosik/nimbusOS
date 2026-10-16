#ifndef NIMBUS_STRING_H
#define NIMBUS_STRING_H
#include "kernel/types.h"

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void   memset32(u32 *dst, u32 v, size_t count);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
int    toupper(int c);
int    tolower(int c);
int    isspace(int c);
int    isdigit(int c);
int    isalpha(int c);
int    isalnum(int c);
int    atoi(const char *s);
long   strtol(const char *s, char **end, int base);

#endif
