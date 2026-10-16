/* NimbusOS libc - system call wrappers (int 0x80, see <syscalls.h>). */
#include <nimbus.h>

long syscall5(long n, long a, long b, long c, long d, long e)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "memory");
    return ret;
}

long write(int fd, const void *buf, size_t len) { return syscall3(SYS_WRITE, fd, buf, len); }
long read(int fd, void *buf, size_t len) { return syscall3(SYS_READ, fd, buf, len); }
void sleep_ms(unsigned ms) { syscall1(SYS_SLEEP, ms); }
unsigned long long ticks_ms(void) { return (unsigned long long)syscall0(SYS_TICKS); }
int getpid(void) { return (int)syscall0(SYS_GETPID); }
void *sbrk(long inc) { return (void *)syscall1(SYS_SBRK, inc); }
void yield(void) { syscall0(SYS_YIELD); }
long readfile(const char *p, void *buf, size_t max) { return syscall3(SYS_READFILE, p, buf, max); }
int writefile(const char *p, const void *buf, size_t len) { return (int)syscall3(SYS_WRITEFILE, p, buf, len); }
long listdir(const char *p, char *buf, size_t max) { return syscall3(SYS_LISTDIR, p, buf, max); }
int mkdir(const char *p) { return (int)syscall1(SYS_MKDIR, p); }
int remove(const char *p) { return (int)syscall1(SYS_REMOVE, p); }
int getcwd(char *buf, size_t max) { return (int)syscall2(SYS_GETCWD, buf, max); }
int gettime(struct nimbus_time *t) { return (int)syscall1(SYS_TIME, t); }
int sysinfo(struct nimbus_sysinfo *s) { return (int)syscall1(SYS_SYSINFO, s); }
