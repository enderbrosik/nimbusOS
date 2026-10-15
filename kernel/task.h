#ifndef NIMBUS_TASK_H
#define NIMBUS_TASK_H
#include "kernel/types.h"

enum task_state { TASK_READY, TASK_SLEEPING, TASK_BLOCKED, TASK_DEAD };

struct task {
    int id;
    char name[32];
    u32 esp;                 /* saved stack pointer (points at struct regs) */
    void *stack;             /* base of kmalloc'ed kernel stack (NULL for boot task) */
    u32 stack_size;
    enum task_state state;
    u64 wake_tick;
    u32 cpu_ticks;           /* timer ticks spent running */
    u32 cpu_ticks_window;    /* ticks in the current 1 s sampling window */
    u32 cpu_percent;         /* usage over the last window */
    void (*entry)(void *);
    void *arg;
    struct task *next;
};

void tasking_init(void);
struct task *task_create(const char *name, void (*entry)(void *), void *arg);
struct task *task_create_ex(const char *name, void (*entry)(void *), void *arg, u32 stack_size);
struct sysmon_stats { u32 heap_used, heap_mapped, free_frames, samples; };
extern struct sysmon_stats g_sysmon;
void task_yield(void);
void task_sleep(u32 ms);
NORETURN void task_exit(void);
struct task *task_current(void);
struct task_info {
    int id;
    char name[32];
    enum task_state state;
    u32 cpu_percent, cpu_ticks, stack_size;
};
int  task_list(struct task_info *out, int max);   /* snapshot of all live tasks */
#endif
