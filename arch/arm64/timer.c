/* NimbusOS - ARM generic timer: CNTPCT gives the time, the EL1 physical
 * timer interrupts 1000 times a second for the scheduler.  The interrupt
 * reaches the core through the GIC-400 on the Pi 4 / 400 and through the
 * ARM local interrupt controller on the Pi 3. */
#include "arch/arm64/pi.h"
#include "drivers/pit.h"
#include "kernel/kprintf.h"
#include "kernel/panic.h"
#include "arch/x86/idt.h"

#define GICD (g_periph + 0x1841000)          /* 0xFF841000 */
#define GICC (g_periph + 0x1842000)
#define TIMER_PPI 30                         /* non-secure EL1 physical timer */

static u64 freq = 1, interval;
static volatile u64 irqs;
uptr (*sched_hook)(uptr sp, int voluntary);

void timer_early(void) { freq = SYSREG(cntfrq_el0); if (!freq) freq = 54000000; }

u64 timer_ticks(void)
{
    u64 c = SYSREG(cntpct_el0);
    return c / freq * 1000 + c % freq * 1000 / freq;
}

void timer_busy_wait(u32 ms)
{
    u64 end = timer_ticks() + ms;
    while (timer_ticks() < end) {}
}

u64 irq_count(void) { return irqs; }

void pit_init(void)
{
    interval = freq / TIMER_HZ;
    if (g_pi_model == 4) {
        mmio_w(GICD + 0x000, 1);                                   /* distributor on (group 1) */
        mmio_w(GICD + 0x100, 1u << TIMER_PPI);                     /* ISENABLER0 */
        u32 pr = mmio_r(GICD + 0x400 + (TIMER_PPI & ~3));
        pr &= ~(0xFFu << ((TIMER_PPI & 3) * 8));
        pr |= 0xA0u << ((TIMER_PPI & 3) * 8);
        mmio_w(GICD + 0x400 + (TIMER_PPI & ~3), pr);
        mmio_w(GICC + 0x004, 0xF0);                                /* priority mask */
        mmio_w(GICC + 0x000, 1);                                   /* CPU interface on */
    }
    mmio_w(g_local + 0x40, 1 << 1);                                /* core 0: CNTPNS IRQ (legacy path) */
    SET_SYSREG(cntp_tval_el0, interval);
    SET_SYSREG(cntp_ctl_el0, 1);
    kprintf("[tmr ] generic timer %u kHz, tick 1 ms (%s)\n", (u32)(freq / 1000),
            g_pi_model == 4 ? "GIC-400" : "local interrupt controller");
}

static uptr on_irq(struct regs *r)
{
    bool tick = false;
    if (g_pi_model == 4) {
        u32 iar = mmio_r(GICC + 0x00C);
        u32 id = iar & 0x3FF;
        if (id == TIMER_PPI) tick = true;
        if (id < 1020) mmio_w(GICC + 0x010, iar);
    }
    if (!tick && (mmio_r(g_local + 0x60) & (1 << 1))) tick = true;
    if (!tick && (SYSREG(cntp_ctl_el0) & 4)) tick = true;          /* ISTATUS: our timer fired */
    if (!tick) return (uptr)r;
    SET_SYSREG(cntp_tval_el0, interval);
    irqs++;
    return sched_hook ? sched_hook((uptr)r, 0) : (uptr)r;
}

static const char *ec_name(u32 ec)
{
    switch (ec) {
    case 0x00: return "unknown instruction";
    case 0x07: return "FP/SIMD access";
    case 0x0E: return "illegal execution state";
    case 0x21: return "instruction abort";
    case 0x22: return "misaligned PC";
    case 0x25: return "data abort";
    case 0x26: return "misaligned stack pointer";
    case 0x3C: return "breakpoint (BRK)";
    default:   return "synchronous exception";
    }
}

/* Called from arch/arm64/vectors.S; returns the stack pointer to resume. */
uptr exc_dispatch(struct regs *r, u64 kind)
{
    if (kind == 1) return on_irq(r);
    if (kind == 0) {
        u64 esr = SYSREG(esr_el1);
        u32 ec = (u32)(esr >> 26);
        if (ec == 0x15) return sched_hook ? sched_hook((uptr)r, 1) : (uptr)r;     /* SVC: task_yield */
        r->pad = esr;
        panic_regs(ec_name(ec), r);
    }
    if (kind == 2) return (uptr)r;
    r->pad = SYSREG(esr_el1);
    panic_regs(kind == 3 ? "SError (asynchronous abort)" : "unexpected exception vector", r);
}
