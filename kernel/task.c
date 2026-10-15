/*
 * NimbusOS - kernel threads and a preemptive round-robin scheduler.
 *
 * Every task has its own kernel stack.  A context switch happens inside the
 * interrupt path: isr_common has saved all registers as a struct regs on the
 * current stack; the scheduler stores that stack pointer in the current task
 * and returns another task's saved stack pointer, and isr_common's epilogue
 * (pop regs + iret) resumes the other task.  New tasks get a hand-built
 * struct regs frame whose EIP is task_trampoline.
 *
 * Preemption: IRQ0 (1 kHz) switches tasks every QUANTUM ticks.
 * Cooperation: task_yield() raises software interrupt 0x30.
 */
#include "kernel/task.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/panic.h"
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "drivers/pit.h"

#define QUANTUM 10
#define STACK_SIZE (32 * 1024)

static struct task boot_task;
static struct task *current, *idle_task;
static int next_id;
static u32 slice;
static u64 window_start;

static void task_trampoline(void)
{
    sti();
    current->entry(current->arg);
    task_exit();
}

static void update_cpu_stats(void)
{
    u64 now = timer_ticks();
    if (now - window_start < 1000) return;
    u32 span = (u32)(now - window_start);
    struct task *t = current;
    do {
        t->cpu_percent = t->cpu_ticks_window * 100 / span;
        t->cpu_ticks_window = 0;
        t = t->next;
    } while (t != current);
    window_start = now;
}

static u32 schedule(u32 esp, int voluntary)
{
    current->esp = esp;
    if (!voluntary) {
        current->cpu_ticks++;
        current->cpu_ticks_window++;
        update_cpu_stats();
        if (++slice < QUANTUM && current->state == TASK_READY) return esp;
    }
    slice = 0;
    u64 now = timer_ticks();
    struct task *t = current;
    do {
        t = t->next;
        if (t->state == TASK_SLEEPING && now >= t->wake_tick) t->state = TASK_READY;
        if (t->state == TASK_READY && t != idle_task) { current = t; return t->esp; }
    } while (t != current);
    if (current->state == TASK_READY && current != idle_task) return current->esp;
    current = idle_task;
    return idle_task->esp;
}

/* Idle task: halts the CPU and reaps dead tasks. */
static void idle_main(void *arg)
{
    for (;;) {
        u32 f = irq_save();
        struct task *prev = current;
        for (struct task *t = current->next; t != current; t = prev->next) {
            if (t->state == TASK_DEAD && t != current) {
                prev->next = t->next;
                kfree(t->stack);
                kfree(t);
                continue;
            }
            prev = t;
        }
        irq_restore(f);
        hlt();
    }
}

void tasking_init(void)
{
    memset(&boot_task, 0, sizeof boot_task);
    strcpy(boot_task.name, "kernel");
    boot_task.id = next_id++;
    boot_task.state = TASK_READY;
    boot_task.next = &boot_task;
    current = &boot_task;
    idle_task = task_create("idle", idle_main, NULL);
    window_start = timer_ticks();
    sched_hook = schedule;
    kprintf("[task] scheduler running (round robin, %u ms quantum)\n", QUANTUM * 1000 / TIMER_HZ);
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    return task_create_ex(name, entry, arg, STACK_SIZE);
}

struct task *task_create_ex(const char *name, void (*entry)(void *), void *arg, u32 stack_size)
{
    struct task *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->stack = kmalloc(stack_size);
    if (!t->stack) { kfree(t); return NULL; }
    t->stack_size = stack_size;
    strlcpy(t->name, name, sizeof t->name);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;

    struct regs *r = (struct regs *)((u8 *)t->stack + stack_size - sizeof(struct regs));
    memset(r, 0, sizeof *r);
    r->gs = r->fs = r->es = r->ds = 0x10;
    r->eip = (u32)task_trampoline;
    r->cs = 0x08;
    r->eflags = 0x202;          /* IF set */
    t->esp = (u32)r;

    u32 f = irq_save();
    t->id = next_id++;
    t->next = current->next;
    current->next = t;
    irq_restore(f);
    klog("[task] created task %d '%s'\n", t->id, t->name);
    return t;
}

void task_yield(void) { __asm__ volatile("int $0x30"); }

void task_sleep(u32 ms)
{
    u32 f = irq_save();
    current->wake_tick = timer_ticks() + ms;
    current->state = TASK_SLEEPING;
    irq_restore(f);
    task_yield();
}

void task_exit(void)
{
    cli();
    klog("[task] task %d '%s' exited\n", current->id, current->name);
    current->state = TASK_DEAD;
    task_yield();
    panic("task_exit: dead task was rescheduled");
}

struct task *task_current(void) { return current; }

int task_list(struct task_info *out, int max)
{
    u32 f = irq_save();
    int n = 0;
    struct task *t = &boot_task;
    do {
        if (n < max && t->state != TASK_DEAD) {
            out[n].id = t->id;
            memcpy(out[n].name, t->name, sizeof out[n].name);
            out[n].state = t->state;
            out[n].cpu_percent = t->cpu_percent;
            out[n].cpu_ticks = t->cpu_ticks;
            out[n].stack_size = t->stack_size;
            n++;
        }
        t = t->next;
    } while (t != &boot_task);
    irq_restore(f);
    return n;
}
