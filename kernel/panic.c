/* NimbusOS - kernel panic: dump registers to serial and screen, then halt. */
#include "kernel/panic.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "drivers/vga_text.h"

void (*panic_display_hook)(const char *text);
static char panic_buf[2048];

static NORETURN void halt_forever(void)
{
    for (;;) { cli(); hlt(); }
}

static void show(void)
{
    klog("\n%s\n", panic_buf);
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
    u32 ebp;
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    int n = snprintf(panic_buf, sizeof panic_buf,
        "*** NimbusOS KERNEL PANIC ***\n\n%s\n\nCR0=%08x CR2=%08x CR3=%08x CR4=%08x\nBacktrace:",
        msg, read_cr0(), read_cr2(), read_cr3(), read_cr4());
    /* Walk saved frame pointers (only meaningful with -fno-omit-frame-pointer). */
    u32 *fp = (u32 *)ebp;
    for (int i = 0; i < 8 && fp && (u32)fp > 0x1000 && (u32)fp < 0x10000000; i++) {
        n += snprintf(panic_buf + n, sizeof panic_buf - n, " %08x", fp[1]);
        fp = (u32 *)fp[0];
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
        "Unhandled exception %u: %s (error code %08x)\n\n"
        "EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n"
        "ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n"
        "EIP=%08x EFLAGS=%08x CS=%04x DS=%04x ES=%04x\n"
        "CR0=%08x CR2=%08x CR3=%08x CR4=%08x\n\n"
        "System halted.",
        r->int_no, what, r->err_code,
        r->eax, r->ebx, r->ecx, r->edx,
        r->esi, r->edi, r->ebp, (u32)r + sizeof(struct regs),
        r->eip, r->eflags, r->cs, r->ds, r->es,
        read_cr0(), read_cr2(), read_cr3(), read_cr4());
    show();
    halt_forever();
}
