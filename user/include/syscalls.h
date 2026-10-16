/*
 * NimbusOS system call numbers and shared structures.  Included by both the
 * kernel (kernel/syscall.c) and the user C library (user/lib).
 *
 * Calling convention (int 0x80): RAX = number, arguments in RDI, RSI, RDX,
 * R10, R8; the result comes back in RAX.  Negative results are errors,
 * errno-style (see strerror() in user/lib/string.c).  All other registers
 * are preserved.
 */
#ifndef NIMBUS_SYSCALLS_H
#define NIMBUS_SYSCALLS_H

#define SYS_EXIT        0   /* (int code)                                 */
#define SYS_WRITE       1   /* (int fd, const void *buf, size_t len) -> n */
#define SYS_READ        2   /* (int fd, void *buf, size_t len) -> n       */
#define SYS_SLEEP       3   /* (unsigned ms)                              */
#define SYS_TICKS       4   /* () -> milliseconds since boot              */
#define SYS_GETPID      5   /* () -> pid                                  */
#define SYS_SBRK        6   /* (long increment) -> old break or -1        */
#define SYS_READFILE    7   /* (const char *path, void *buf, size_t max) -> size (may exceed max) */
#define SYS_WRITEFILE   8   /* (const char *path, const void *buf, size_t len) -> 0 */
#define SYS_LISTDIR     9   /* (const char *path, char *buf, size_t max) -> bytes, "name\n" per entry, dirs end in '/' */
#define SYS_TIME       10   /* (struct nimbus_time *) -> 0                 */
#define SYS_SYSINFO    11   /* (struct nimbus_sysinfo *) -> 0              */
#define SYS_YIELD      12   /* ()                                          */
#define SYS_GETCWD     13   /* (char *buf, size_t max) -> length           */
#define SYS_MKDIR      14   /* (const char *path) -> 0                     */
#define SYS_REMOVE     15   /* (const char *path) -> 0                     */
#define SYS_COUNT      16

#define E_NOENT  -2
#define E_IO     -5
#define E_NOMEM -12
#define E_FAULT -14
#define E_INVAL -22
#define E_NOSYS -38

struct nimbus_time {
    int year, month, day, hour, minute, second;   /* local time */
};

struct nimbus_sysinfo {
    unsigned long long uptime_ms;
    unsigned int total_kb, free_kb;      /* physical memory */
    unsigned int tasks;                  /* live kernel tasks */
    unsigned int processes;              /* running user programs */
    char os[32];                         /* "NimbusOS 0.2 x86_64" */
    char cpu[49];                        /* CPUID brand string */
};

#endif
