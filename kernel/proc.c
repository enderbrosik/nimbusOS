/*
 * NimbusOS - user processes: ELF64 loading, ring 3 entry, console I/O and
 * the file system proxy (see kernel/proc.h).
 *
 * ELF format: System V ABI gABI "ELF-64 Object File Format" (Elf64_Ehdr,
 * Elf64_Phdr, PT_LOAD).  Entering ring 3: a hand-built interrupt frame
 * with the user code/stack selectors is popped by isr_return's iretq
 * (Intel SDM Vol. 3A section 6.14 "Exception and Interrupt Handling in
 * 64-bit Mode").
 */
#include "kernel/proc.h"
#include "kernel/task.h"
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/panic.h"
#include "arch/x86/io.h"
#include "arch/x86/gdt.h"
#include "fs/vfs.h"
#include "fs/nbfs.h"
#include "user/include/syscalls.h"

#define OUT_SIZE    8192
#define IN_SIZE     512
#define STACK_PAGES 64                      /* 256 KiB user stack */
#define BRK_LIMIT   (USER_BASE + 0x40000000ull)   /* 1 GiB of heap at most */
#define RESERVE_FRAMES 1024                 /* never let programs take the last 4 MiB */

enum { P_RUNNING, P_EXITED };
enum { FS_IDLE, FS_PENDING, FS_DONE };

struct proc {
    int pid;
    char name[32];
    char cwd[NBFS_PATH_MAX];
    u64 space;
    uptr entry, brk, brk_mapped;
    struct task *task;
    volatile int state;
    volatile bool killed;
    int exit_code;
    char fault[160];
    int refs;                       /* owner + running task */
    /* console */
    char out[OUT_SIZE];
    volatile u32 out_head, out_tail;
    char in[IN_SIZE];
    volatile u32 in_len;
    volatile bool in_ready, want_input;
    /* file system request, served by the GUI task */
    volatile int fs_state;
    int fs_op;
    char fs_path[NBFS_PATH_MAX];
    void *fs_buf;
    u32 fs_len;
    long fs_result;
};

static int next_pid = 1;
static int live;

/* ---- ELF64 ------------------------------------------------------------- */

typedef struct {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED elf64_ehdr;

typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} PACKED elf64_phdr;

#define PT_LOAD 1
#define ET_EXEC 2
#define EM_X86_64 62

/* Map (if needed) and return a pointer, through the identity map, to the
 * byte at user address `va` of space `space`. */
static u8 *user_page(u64 space, uptr va, bool create)
{
    u64 phys = vmm_space_translate(space, va & ~0xFFFull);
    if (!phys) {
        if (!create) return NULL;
        if (pmm_free_frames() < RESERVE_FRAMES) return NULL;
        u32 frame = pmm_alloc();
        if (!frame) return NULL;
        memset((void *)(uptr)frame, 0, 4096);
        if (!vmm_space_map(space, va & ~0xFFFull, frame, PTE_WRITE)) { pmm_free(frame); return NULL; }
        phys = frame;
    }
    return (u8 *)(uptr)phys + (va & 0xFFF);
}

/* Copy into another address space (it does not have to be loaded). */
static bool copy_to_space(u64 space, uptr va, const void *src, u64 len)
{
    const u8 *s = src;
    while (len) {
        u8 *d = user_page(space, va, true);
        if (!d) return false;
        u64 n = MIN(len, 4096 - (va & 0xFFF));
        if (s) { memcpy(d, s, n); s += n; }
        va += n; len -= n;
    }
    return true;
}

static const char *load_elf(struct proc *p, const u8 *elf, u32 len)
{
    if (len < sizeof(elf64_ehdr)) return "not an executable (too small)";
    const elf64_ehdr *h = (const elf64_ehdr *)elf;
    if (memcmp(h->ident, "\x7F" "ELF", 4)) return "not an ELF executable";
    if (h->ident[4] != 2 || h->ident[5] != 1) return "not a 64-bit little-endian ELF file";
    if (h->type != ET_EXEC || h->machine != EM_X86_64) return "not an x86-64 executable";
    if (h->phentsize != sizeof(elf64_phdr) || h->phoff + (u64)h->phnum * sizeof(elf64_phdr) > len)
        return "corrupt program headers";
    uptr top = USER_BASE;
    for (int i = 0; i < h->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(elf + h->phoff + i * sizeof(elf64_phdr));
        if (ph->type != PT_LOAD || !ph->memsz) continue;
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > BRK_LIMIT || ph->memsz < ph->filesz ||
            ph->offset + ph->filesz > len)
            return "segment outside the user address range";
        if (!copy_to_space(p->space, ph->vaddr, elf + ph->offset, ph->filesz) ||
            !copy_to_space(p->space, ph->vaddr + ph->filesz, NULL, ph->memsz - ph->filesz))
            return "out of memory";
        top = MAX(top, ph->vaddr + ph->memsz);
    }
    if (h->entry < USER_BASE || h->entry >= top) return "bad entry point";
    p->entry = h->entry;
    p->brk = top;
    p->brk_mapped = ALIGN_UP(top, 4096);
    return NULL;
}

/* Lay out argc/argv at the top of the user stack; returns the initial RSP
 * and the user address of argv. */
static bool setup_stack(struct proc *p, int argc, char **argv, uptr *rsp, uptr *uargv)
{
    for (uptr va = USER_STACK_TOP - STACK_PAGES * 4096; va < USER_STACK_TOP; va += 4096)
        if (!user_page(p->space, va, true)) return false;
    uptr sp = USER_STACK_TOP;
    uptr ptrs[33];
    argc = MIN(argc, 32);
    for (int i = argc - 1; i >= 0; i--) {
        u32 n = strlen(argv[i]) + 1;
        sp -= n;
        copy_to_space(p->space, sp, argv[i], n);
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;
    sp &= ~(uptr)15;
    sp -= (argc + 1) * sizeof(uptr);
    sp &= ~(uptr)15;
    copy_to_space(p->space, sp, ptrs, (argc + 1) * sizeof(uptr));
    *uargv = sp;
    *rsp = sp - 16;
    return true;
}

/* ---- starting a process ----------------------------------------------- */

struct start { struct proc *p; uptr rsp, argv; int argc; };

static NORETURN void enter_user(uptr rip, uptr rsp, u64 argc, u64 argv)
{
    struct regs r;
    memset(&r, 0, sizeof r);
    r.rip = rip;
    r.cs = USER_CS;
    r.rflags = 0x202;           /* IF */
    r.rsp = rsp;
    r.ss = USER_DS;
    r.rdi = argc;
    r.rsi = argv;
    cli();
    __asm__ volatile("mov %0, %%rsp\n\tjmp isr_return" :: "r"(&r) : "memory");
    __builtin_unreachable();
}

static void proc_main(void *arg)
{
    struct start st = *(struct start *)arg;
    kfree(arg);
    struct proc *p = st.p;
    u32 f = irq_save();
    task_current()->space = p->space;
    task_current()->proc = p;
    vmm_space_switch(p->space);
    irq_restore(f);
    klog("[proc] pid %d '%s' entering ring 3 at %llx\n", p->pid, p->name, (u64)p->entry);
    enter_user(p->entry, st.rsp, st.argc, st.argv);
}

struct proc *proc_spawn(const u8 *elf, u32 len, int argc, char **argv, const char *cwd, const char **why)
{
    *why = NULL;
    if (live >= PROC_MAX) { *why = "too many programs running"; return NULL; }
    struct proc *p = kzalloc(sizeof *p);
    if (!p) { *why = "out of memory"; return NULL; }
    p->space = vmm_space_create();
    if (!p->space) { kfree(p); *why = "out of memory"; return NULL; }
    const char *base = strrchr(argv[0], '/');
    strlcpy(p->name, base ? base + 1 : argv[0], sizeof p->name);
    char *dot = strrchr(p->name, '.');
    if (dot && !strcasecmp(dot, ".elf")) *dot = 0;
    strlcpy(p->cwd, cwd, sizeof p->cwd);
    struct start *st = kzalloc(sizeof *st);
    const char *e = st ? load_elf(p, elf, len) : "out of memory";
    if (!e && !setup_stack(p, argc, argv, &st->rsp, &st->argv)) e = "out of memory";
    if (e) {
        vmm_space_destroy(p->space);
        kfree(st); kfree(p);
        *why = e;
        return NULL;
    }
    st->p = p;
    st->argc = MIN(argc, 32);
    p->refs = 2;
    p->state = P_RUNNING;
    u32 f = irq_save();
    p->pid = next_pid++;
    live++;
    irq_restore(f);
    p->task = task_create(p->name, proc_main, st);
    if (!p->task) {
        live--;
        vmm_space_destroy(p->space);
        kfree(st); kfree(p);
        *why = "cannot create task";
        return NULL;
    }
    kprintf("[proc] started pid %d '%s' (%u KiB image)\n", p->pid, p->name, len / 1024);
    return p;
}

/* ---- owner (GUI task) side --------------------------------------------- */

u32 proc_read_output(struct proc *p, char *buf, u32 max)
{
    u32 n = 0;
    while (n < max && p->out_tail != p->out_head) {
        buf[n++] = p->out[p->out_tail % OUT_SIZE];
        p->out_tail++;
    }
    return n;
}

bool proc_wants_input(struct proc *p) { return p->state == P_RUNNING && p->want_input && !p->in_ready; }

void proc_send_input(struct proc *p, const char *line, u32 len)
{
    len = MIN(len, IN_SIZE);
    memcpy(p->in, line, len);
    p->in_len = len;
    p->in_ready = true;
}

static void put_list(struct proc *p, const char *path)
{
    struct vfs_item *items;
    int n = vfs_list(path, &items);
    if (n < 0) { p->fs_result = n; return; }
    u32 pos = 0;
    for (int i = 0; i < n; i++) {
        u32 l = strlen(items[i].name);
        if (pos + l + 2 > p->fs_len) break;
        memcpy((char *)p->fs_buf + pos, items[i].name, l);
        pos += l;
        if (items[i].type == NBFS_TYPE_DIR) ((char *)p->fs_buf)[pos++] = '/';
        ((char *)p->fs_buf)[pos++] = '\n';
    }
    kfree(items);
    p->fs_result = pos;
}

bool proc_service(struct proc *p)
{
    if (p->fs_state != FS_PENDING) return false;
    switch (p->fs_op) {
    case SYS_READFILE: {
        char *data; u32 size;
        int r = vfs_read_all(p->fs_path, &data, &size);
        if (r) { p->fs_result = r; break; }
        memcpy(p->fs_buf, data, MIN(size, p->fs_len));
        kfree(data);
        p->fs_result = size;
        break;
    }
    case SYS_WRITEFILE: p->fs_result = vfs_write_all(p->fs_path, p->fs_buf, p->fs_len); break;
    case SYS_LISTDIR:   put_list(p, p->fs_path); break;
    case SYS_MKDIR:     p->fs_result = nbfs_mkdir(ROOTFS, p->fs_path); break;
    case SYS_REMOVE:    p->fs_result = nbfs_unlink(ROOTFS, p->fs_path); break;
    default:            p->fs_result = E_NOSYS; break;
    }
    if (p->fs_result > 0 && p->fs_op != SYS_READFILE && p->fs_op != SYS_LISTDIR) p->fs_result = 0;
    p->fs_state = FS_DONE;
    return true;
}

bool proc_finished(struct proc *p) { return p->state == P_EXITED && p->out_tail == p->out_head; }
int proc_exit_code(struct proc *p) { return p->exit_code; }
const char *proc_fault(struct proc *p) { return p->fault[0] ? p->fault : NULL; }
int proc_pid(struct proc *p) { return p->pid; }
int proc_count(void) { return live; }

void proc_kill(struct proc *p)
{
    if (p->state == P_RUNNING) { p->killed = true; klog("[proc] kill pid %d\n", p->pid); }
}

static void put_ref(struct proc *p)
{
    u32 f = irq_save();
    bool last = --p->refs == 0;
    irq_restore(f);
    if (last) kfree(p);
}

void proc_release(struct proc *p)
{
    proc_kill(p);
    /* a pending file request must not be left waiting for an owner */
    if (p->fs_state == FS_PENDING) { p->fs_result = E_IO; p->fs_state = FS_DONE; }
    put_ref(p);
}

/* The task is gone: free its address space (we are in the idle task, with
 * the kernel address space loaded). */
void proc_task_reaped(struct proc *p)
{
    vmm_space_destroy(p->space);
    p->space = 0;
    klog("[proc] pid %d reaped, %u frames free\n", p->pid, pmm_free_frames());
    put_ref(p);
}

/* ---- process side ------------------------------------------------------ */

struct proc *proc_current(void) { return task_current()->proc; }
bool proc_killed_current(void) { struct proc *p = proc_current(); return p && p->killed; }

void proc_exit_current(int code)
{
    struct proc *p = proc_current();
    cli();
    p->exit_code = code;
    p->state = P_EXITED;
    live--;
    klog("[proc] pid %d '%s' exited with code %d%s%s\n", p->pid, p->name, code,
         p->fault[0] ? ": " : "", p->fault);
    task_exit();
}

bool proc_user_range_ok(uptr addr, u64 len)
{
    struct proc *p = proc_current();
    if (!p || addr < USER_BASE || addr + len < addr || addr + len > USER_STACK_TOP) return false;
    for (uptr va = addr & ~0xFFFull; va < addr + len; va += 4096)
        if (!vmm_space_translate(p->space, va)) return false;
    return true;
}

long proc_sys_write(const char *ubuf, u64 len)
{
    struct proc *p = proc_current();
    if (!proc_user_range_ok((uptr)ubuf, len)) return E_FAULT;
    for (u64 i = 0; i < len; i++) {
        while (p->out_head - p->out_tail >= OUT_SIZE) {       /* full: wait for the Terminal */
            if (p->killed) return i;
            task_sleep(5);
        }
        p->out[p->out_head % OUT_SIZE] = ubuf[i];
        p->out_head++;
    }
    return len;
}

long proc_sys_read(char *ubuf, u64 len)
{
    struct proc *p = proc_current();
    if (!proc_user_range_ok((uptr)ubuf, len)) return E_FAULT;
    p->want_input = true;
    while (!p->in_ready) {
        if (p->killed) return 0;
        task_sleep(10);
    }
    u32 n = MIN(len, p->in_len);
    memcpy(ubuf, p->in, n);
    p->in_ready = false;
    p->want_input = false;
    return n;
}

long proc_sys_sbrk(i64 inc)
{
    struct proc *p = proc_current();
    uptr old = p->brk;
    if (inc < 0) { if ((u64)-inc > p->brk - USER_BASE) return -1; p->brk += inc; return old; }
    uptr nb = p->brk + inc;
    if (nb > BRK_LIMIT) return -1;
    while (p->brk_mapped < nb) {
        if (!user_page(p->space, p->brk_mapped, true)) return -1;
        p->brk_mapped += 4096;
    }
    p->brk = nb;
    return old;
}

long proc_sys_getcwd(char *ubuf, u64 max)
{
    struct proc *p = proc_current();
    u32 n = strlen(p->cwd);
    if (!max || !proc_user_range_ok((uptr)ubuf, max)) return E_FAULT;
    if (n + 1 > max) return E_INVAL;
    memcpy(ubuf, p->cwd, n + 1);
    return n;
}

/* Copy a NUL-terminated string from user memory. */
static bool copy_user_str(char *dst, const char *usrc, u32 max)
{
    for (u32 i = 0; i < max; i++) {
        if (!proc_user_range_ok((uptr)usrc + i, 1)) return false;
        dst[i] = usrc[i];
        if (!dst[i]) return true;
    }
    return false;
}

long proc_sys_fs(int op, const char *upath, void *ubuf, u64 len)
{
    struct proc *p = proc_current();
    char rel[NBFS_PATH_MAX];
    if (!copy_user_str(rel, upath, sizeof rel)) return E_FAULT;
    if (len > 16 * 1024 * 1024) return E_INVAL;
    if (len && !proc_user_range_ok((uptr)ubuf, len)) return E_FAULT;
    /* Bounce buffer: the GUI task runs in the kernel address space and
     * cannot see this process's memory. */
    void *kbuf = len ? kmalloc(len) : NULL;
    if (len && !kbuf) return E_NOMEM;
    if (op == SYS_WRITEFILE && len) memcpy(kbuf, ubuf, len);
    nbfs_path_join(p->cwd, rel, p->fs_path, sizeof p->fs_path);
    p->fs_op = op;
    p->fs_buf = kbuf;
    p->fs_len = len;
    p->fs_state = FS_PENDING;
    /* The owner either serves the request or, when it lets go of the
     * process, completes it with an error - so this wait always ends. */
    while (p->fs_state != FS_DONE) task_sleep(2);
    long r = p->fs_result;
    if (op != SYS_WRITEFILE && r > 0 && len) memcpy(ubuf, kbuf, MIN((u64)r, len));
    kfree(kbuf);
    p->fs_state = FS_IDLE;
    return r;
}

/* ---- faults and kills ---------------------------------------------------- */

static uptr user_fault(struct regs *r, const char *what)
{
    struct proc *p = proc_current();
    if (!p) panic_regs(what, r);
    if (r->int_no == 14)
        snprintf(p->fault, sizeof p->fault, "%s at address %llx (%s, %s) rip=%llx", what, read_cr2(),
                 r->err_code & 1 ? "protection violation" : "page not present",
                 r->err_code & 2 ? "write" : "read", r->rip);
    else
        snprintf(p->fault, sizeof p->fault, "%s at rip=%llx", what, r->rip);
    proc_exit_current(139);
}

/* Every return to ring 3 passes here: a killed program (Ctrl+C, window
 * closed) ends even if it never makes a system call. */
static void user_return(struct regs *r)
{
    (void)r;
    if (proc_killed_current()) proc_exit_current(130);
}

void proc_init(void)
{
    user_fault_hook = user_fault;
    user_return_hook = user_return;
}
