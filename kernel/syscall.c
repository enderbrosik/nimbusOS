/*
 * NimbusOS - system call dispatcher (int 0x80, see user/include/syscalls.h).
 *
 * The interrupt gate enters with interrupts disabled; system calls run with
 * them enabled, so a call may sleep (waiting for input, for the Terminal to
 * drain output, for a file request) and the scheduler keeps running other
 * tasks meanwhile.
 */
#include "kernel/proc.h"
#include "kernel/task.h"
#include "kernel/pmm.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "gui/taskbar.h"
#include "user/include/syscalls.h"

static char cpu_brand[49];

static void read_cpu_brand(void)
{
    u32 *b = (u32 *)cpu_brand;
    u32 max;
    __asm__ volatile("cpuid" : "=a"(max) : "a"(0x80000000) : "ebx", "ecx", "edx");
    if (max < 0x80000004) { strcpy(cpu_brand, "x86-64 processor"); return; }
    for (u32 leaf = 0; leaf < 3; leaf++)
        __asm__ volatile("cpuid" : "=a"(b[leaf * 4]), "=b"(b[leaf * 4 + 1]), "=c"(b[leaf * 4 + 2]), "=d"(b[leaf * 4 + 3])
                         : "a"(0x80000002 + leaf));
    cpu_brand[48] = 0;
    char *s = cpu_brand;
    while (*s == ' ') s++;
    memmove(cpu_brand, s, strlen(s) + 1);
}

static long sys_sysinfo(struct nimbus_sysinfo *u)
{
    if (!proc_user_range_ok((uptr)u, sizeof *u)) return E_FAULT;
    struct nimbus_sysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime_ms = timer_ticks() * 1000 / TIMER_HZ;
    si.total_kb = pmm_total_frames() * 4;
    si.free_kb = pmm_free_frames() * 4;
    struct task_info ti[64];
    si.tasks = task_list(ti, 64);
    si.processes = proc_count();
    strcpy(si.os, "NimbusOS 0.2 x86_64");
    strlcpy(si.cpu, cpu_brand, sizeof si.cpu);
    memcpy(u, &si, sizeof si);
    return 0;
}

static long sys_time(struct nimbus_time *u)
{
    if (!proc_user_range_ok((uptr)u, sizeof *u)) return E_FAULT;
    struct rtc_time t;
    clock_now(&t);
    struct nimbus_time nt = { t.year, t.month, t.day, t.hour, t.minute, t.second };
    memcpy(u, &nt, sizeof nt);
    return 0;
}

static long dispatch(struct regs *r)
{
    u64 a = r->rdi, b = r->rsi, c = r->rdx;
    switch (r->rax) {
    case SYS_EXIT:      proc_exit_current((int)a);
    case SYS_WRITE:     return a == 1 || a == 2 ? proc_sys_write((const char *)b, c) : E_INVAL;
    case SYS_READ:      return a == 0 ? proc_sys_read((char *)b, c) : E_INVAL;
    case SYS_SLEEP:     task_sleep((u32)MIN(a, 3600000)); return 0;
    case SYS_TICKS:     return (long)(timer_ticks() * 1000 / TIMER_HZ);
    case SYS_GETPID:    return proc_pid(proc_current());
    case SYS_SBRK:      return proc_sys_sbrk((i64)a);
    case SYS_READFILE:
    case SYS_WRITEFILE:
    case SYS_LISTDIR:   return proc_sys_fs((int)r->rax, (const char *)a, (void *)b, c);
    case SYS_MKDIR:
    case SYS_REMOVE:    return proc_sys_fs((int)r->rax, (const char *)a, NULL, 0);
    case SYS_TIME:      return sys_time((struct nimbus_time *)a);
    case SYS_SYSINFO:   return sys_sysinfo((struct nimbus_sysinfo *)a);
    case SYS_YIELD:     task_yield(); return 0;
    case SYS_GETCWD:    return proc_sys_getcwd((char *)a, b);
    default:            return E_NOSYS;
    }
}

static uptr syscall_entry(struct regs *r)
{
    if (!(r->cs & 3) || !proc_current()) return (uptr)r;   /* only from user programs */
    sti();
    r->rax = (u64)dispatch(r);
    cli();
    return (uptr)r;
}

void syscall_init(void)
{
    read_cpu_brand();
    proc_init();
    syscall_hook = syscall_entry;
    kprintf("[sys ] system calls on int 0x80, %d calls; user programs at %llx\n", SYS_COUNT,
            (u64)0x8000000000ull);
}
