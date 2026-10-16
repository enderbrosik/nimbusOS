/* NimbusOS libc - <stdio.h>
 * stdout/stderr go to the Terminal that started the program; stdin is the
 * line typed into that Terminal.  Files are read and written whole
 * (readfile/writefile in <nimbus.h>); FILE streams cover only the console. */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
typedef struct { int fd; } FILE;
extern FILE *stdin, *stdout, *stderr;
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  vprintf(const char *fmt, va_list ap);
int  vfprintf(FILE *f, const char *fmt, va_list ap);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  puts(const char *s);
int  fputs(const char *s, FILE *f);
int  putchar(int c);
int  fputc(int c, FILE *f);
int  getchar(void);
char *fgets(char *buf, int n, FILE *f);
int  fflush(FILE *f);
#endif
