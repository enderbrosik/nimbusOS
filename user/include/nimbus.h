/* NimbusOS libc - NimbusOS-specific calls. */
#ifndef _NIMBUS_H
#define _NIMBUS_H
#include <stddef.h>
#include "syscalls.h"
long syscall5(long n, long a, long b, long c, long d, long e);
#define syscall0(n)          syscall5(n, 0, 0, 0, 0, 0)
#define syscall1(n, a)       syscall5(n, (long)(a), 0, 0, 0, 0)
#define syscall2(n, a, b)    syscall5(n, (long)(a), (long)(b), 0, 0, 0)
#define syscall3(n, a, b, c) syscall5(n, (long)(a), (long)(b), (long)(c), 0, 0)

long  write(int fd, const void *buf, size_t len);
long  read(int fd, void *buf, size_t len);
void  sleep_ms(unsigned ms);
unsigned long long ticks_ms(void);
int   getpid(void);
void *sbrk(long increment);
void  yield(void);
/* Whole-file I/O on the NimbusFS system volume.  Paths are absolute or
 * relative to the Terminal's current directory. */
long  readfile(const char *path, void *buf, size_t max);  /* file size, or <0 error */
int   writefile(const char *path, const void *buf, size_t len);
long  listdir(const char *path, char *buf, size_t max);   /* "name\n" lines, dirs end in '/' */
int   mkdir(const char *path);
int   remove(const char *path);
int   getcwd(char *buf, size_t max);
int   gettime(struct nimbus_time *t);
int   sysinfo(struct nimbus_sysinfo *s);
#endif
