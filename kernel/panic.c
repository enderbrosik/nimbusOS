/* NimbusOS - kernel panic: dump registers to serial and screen, then halt. */
#include "kernel/panic.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "drivers/vga_text.h"

void (*panic_display_hook)(const char *text);
static char panic_buf[4096];

static NORETURN void halt_forever(void)
{
    for (;;) { cli(); hlt(); }
}

static void show(void)
{
    /* line by line: klog formats into a fixed-size buffer */
    klog("\n");
    for (const char *p = panic_buf; *p;) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        klog("%.*s\n", n, p);
        p += n + (nl ? 1 : 0);
    }
    if (panic_display_hook) {
        panic_display_hook(panic_buf);
    } else {
        vga_setcolor(VGA_WHITE, VGA_BLUE);
        vga_clear();
        vga_write(panic_buf);
    }
}

void panic(const char *fmt, ...)
{
    cli();
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    uptr rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    int n = snprintf(panic_buf, sizeof panic_buf,
        "*** NimbusOS KERNEL PANIC ***\n\n%s\n\nCR0=%08llx CR2=%016llx CR3=%08llx CR4=%08llx\nBacktrace:",
        msg, read_cr0(), read_cr2(), read_cr3(), read_cr4());
    /* Walk saved frame pointers (only meaningful with -fno-omit-frame-pointer). */
    uptr *fp = (uptr *)rbp;
    for (int i = 0; i < 8 && fp && (uptr)fp > 0x1000 && (uptr)fp < 0x100000000ull; i++) {
        n += snprintf(panic_buf + n, sizeof panic_buf - n, " %llx", (u64)fp[1]);
        fp = (uptr *)fp[0];
    }
    snprintf(panic_buf + n, sizeof panic_buf - n, "\n\nSystem halted.");
    show();
    halt_forever();
}

void panic_regs(const char *what, struct regs *r)
{
    cli();
    snprintf(panic_buf, sizeof panic_buf,
        "*** NimbusOS KERNEL PANIC ***\n\n"
        "Unhandled exception %u: %s (error code %llx)\n\n"
        "RAX=%016llx RBX=%016llx RCX=%016llx\n"
        "RDX=%016llx RSI=%016llx RDI=%016llx\n"
        "RBP=%016llx RSP=%016llx R8 =%016llx\n"
        "R9 =%016llx R10=%016llx R11=%016llx\n"
        "R12=%016llx R13=%016llx R14=%016llx\n"
        "R15=%016llx RIP=%016llx\n"
        "RFLAGS=%08llx CS=%04llx SS=%04llx\n"
        "CR0=%08llx CR2=%016llx CR3=%08llx CR4=%08llx\n\n"
        "System halted.",
        (u32)r->int_no, what, r->err_code,
        r->rax, r->rbx, r->rcx, r->rdx, r->rsi, r->rdi,
        r->rbp, r->rsp, r->r8, r->r9, r->r10, r->r11,
        r->r12, r->r13, r->r14, r->r15, r->rip,
        r->rflags, r->cs, r->ss,
        read_cr0(), read_cr2(), read_cr3(), read_cr4());
    show();
    halt_forever();
}
