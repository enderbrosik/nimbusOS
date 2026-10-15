#ifndef NIMBUS_PANIC_H
#define NIMBUS_PANIC_H
#include "kernel/types.h"
struct regs;
NORETURN void panic(const char *fmt, ...);
NORETURN void panic_regs(const char *what, struct regs *r);
/* Graphics layer installs this to paint a "blue screen" with the text. */
extern void (*panic_display_hook)(const char *text);
#define ASSERT(c) do { if (!(c)) panic("ASSERT failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)
#endif
