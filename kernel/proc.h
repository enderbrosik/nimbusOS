/*
 * NimbusOS - user processes.
 *
 * A process is a kernel task that runs a static ELF64 program in ring 3 in
 * its own address space.  It talks to the Terminal that started it through
 * an output ring buffer and an input line, and asks the GUI task (the only
 * task allowed to touch the file system) to do file operations for it.
 * All proc_* functions without a "_current" suffix are called from the
 * GUI task.
 */
#ifndef NIMBUS_PROC_H
#define NIMBUS_PROC_H
#include "kernel/types.h"
#include "arch/x86/idt.h"

#define PROC_MAX 16

struct proc;

/* Load `elf` (len bytes) and start it.  argv[0] is the program name; cwd is
 * the directory relative paths are resolved against.  Returns NULL and sets
 * *err to a negative error / *why to a message on failure. */
struct proc *proc_spawn(const u8 *elf, u32 len, int argc, char **argv, const char *cwd, const char **why);
/* Drain program output; returns bytes copied (0 when nothing is pending). */
u32  proc_read_output(struct proc *p, char *buf, u32 max);
bool proc_wants_input(struct proc *p);
void proc_send_input(struct proc *p, const char *line, u32 len);
/* Perform a pending file system request (call regularly from the GUI). */
bool proc_service(struct proc *p);
bool proc_finished(struct proc *p);          /* exited and all output drained */
int  proc_exit_code(struct proc *p);
const char *proc_fault(struct proc *p);       /* NULL unless killed by a CPU exception */
int  proc_pid(struct proc *p);
void proc_kill(struct proc *p);
void proc_release(struct proc *p);            /* owner is done with it */
int  proc_count(void);                        /* running processes */

void proc_init(void);
void syscall_init(void);                      /* kernel/syscall.c */
/* Called from task.c when a dead task that ran a process is reaped. */
void proc_task_reaped(struct proc *p);
/* Process side (current task), used by kernel/syscall.c */
struct proc *proc_current(void);
NORETURN void proc_exit_current(int code);
long proc_sys_write(const char *ubuf, u64 len);
long proc_sys_read(char *ubuf, u64 len);
long proc_sys_sbrk(i64 inc);
long proc_sys_fs(int op, const char *upath, void *ubuf, u64 len);
long proc_sys_getcwd(char *ubuf, u64 max);
bool proc_user_range_ok(uptr addr, u64 len);
bool proc_killed_current(void);
#endif
