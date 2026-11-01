/* NimbusOS - kernel panic on the Pi: the message and registers go to the
 * serial log, the screen and (immediately) NIMBUS.LOG on the SD card. */
#include "arch/arm64/pi.h"
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
    klog("\n");
    for (const char *p = panic_buf; *p;) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        klog("%.*s\n", n, p);
        p += n + (nl ? 1 : 0);
    }
    sdlog_flush();
    if (panic_display_hook) panic_display_hook(panic_buf);
}

void panic(const char *fmt, ...)
{
    cli();
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    u64 *fp = __builtin_frame_address(0);
    int n = snprintf(panic_buf, sizeof panic_buf, "*** NimbusOS KERNEL PANIC ***\n\n%s\n\nBacktrace:", msg);
    for (int i = 0; i < 8 && fp && (uptr)fp > 0x1000 && (uptr)fp < 0x100000000ull; i++) {
        n += snprintf(panic_buf + n, sizeof panic_buf - n, " %llx", fp[1]);
        fp = (u64 *)fp[0];
    }
    snprintf(panic_buf + n, sizeof panic_buf - n, "\n\nSystem halted.");
    show();
    halt_forever();
}

void panic_regs(const char *what, struct regs *r)
{
    cli();
    int n = snprintf(panic_buf, sizeof panic_buf,
        "*** NimbusOS KERNEL PANIC ***\n\nUnhandled exception: %s\nESR=%08llx FAR=%016llx ELR=%016llx SPSR=%08llx\n\n",
        what, r->pad, read_cr2(), r->elr, r->spsr);
    for (int i = 0; i < 31; i++)
        n += snprintf(panic_buf + n, sizeof panic_buf - n, "X%-2d=%016llx%s", i, r->x[i], i % 3 == 2 ? "\n" : "  ");
    snprintf(panic_buf + n, sizeof panic_buf - n, "\nSP=%016llx\n\nSystem halted.", (u64)(uptr)(r + 1));
    show();
    halt_forever();
}
