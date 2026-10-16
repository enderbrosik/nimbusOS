/*
 * NimbusOS - Interrupt Descriptor Table, 8259A PIC and interrupt dispatch.
 *
 * Gate format: Intel SDM Vol. 3A section 6.11 "IDT Descriptors".
 * PIC remapping: OSDev wiki "8259 PIC" / Intel 8259A datasheet: the master
 * PIC is moved to vectors 0x20-0x27 and the slave to 0x28-0x2F so IRQs do
 * not collide with CPU exceptions.
 */
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "kernel/panic.h"
#include "kernel/kprintf.h"

#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

/* 64-bit gate: Intel SDM Vol. 3A section 6.14.1 "64-Bit Mode IDT". */
struct idt_entry {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type;       /* P | DPL | 0 | gate type (0xE = interrupt gate) */
    u16 off_mid;
    u32 off_hi;
    u32 reserved;
} PACKED;

struct idt_ptr { u16 limit; u64 base; } PACKED;

static struct idt_entry idt[256];
static struct idt_ptr ip;
static irq_handler_t irq_handlers[16];
extern u64 isr_table[256];
uptr (*sched_hook)(uptr sp, int voluntary);
uptr (*syscall_hook)(struct regs *r);
uptr (*user_fault_hook)(struct regs *r, const char *what);
void (*user_return_hook)(struct regs *r);

static const char *exc_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND Range Exceeded",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FPU Error", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security", "Reserved"
};

static void set_gate(int n, u64 handler, u8 type)
{
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].ist = 0;
    idt[n].type = type;
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi = handler >> 32;
    idt[n].reserved = 0;
}

static void pic_remap(void)
{
    u8 m1 = 0xFF, m2 = 0xFF;            /* start with everything masked */
    outb(PIC1_CMD, 0x11); io_wait();    /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, IRQ_BASE); io_wait();      /* ICW2: vector offset */
    outb(PIC2_DAT, IRQ_BASE + 8); io_wait();
    outb(PIC1_DAT, 4); io_wait();       /* ICW3: slave on IRQ2 */
    outb(PIC2_DAT, 2); io_wait();       /* ICW3: slave cascade identity */
    outb(PIC1_DAT, 0x01); io_wait();    /* ICW4: 8086 mode */
    outb(PIC2_DAT, 0x01); io_wait();
    outb(PIC1_DAT, m1);
    outb(PIC2_DAT, m2);
}

void pic_unmask(int irq)
{
    if (irq < 8) outb(PIC1_DAT, inb(PIC1_DAT) & ~(1 << irq));
    else {
        outb(PIC2_DAT, inb(PIC2_DAT) & ~(1 << (irq - 8)));
        outb(PIC1_DAT, inb(PIC1_DAT) & ~(1 << 2));   /* cascade */
    }
}

void pic_mask(int irq)
{
    if (irq < 8) outb(PIC1_DAT, inb(PIC1_DAT) | (1 << irq));
    else outb(PIC2_DAT, inb(PIC2_DAT) | (1 << (irq - 8)));
}

void irq_register(int irq, irq_handler_t h)
{
    irq_handlers[irq] = h;
    pic_unmask(irq);
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++) set_gate(i, isr_table[i], 0x8E);
    set_gate(SYSCALL_VECTOR, isr_table[SYSCALL_VECTOR], 0xEE);   /* DPL 3: callable from ring 3 */
    ip.limit = sizeof(idt) - 1;
    ip.base = (u64)&idt;
    __asm__ volatile("lidt %0" :: "m"(ip));
    pic_remap();
}

uptr isr_dispatch(struct regs *r)
{
    uptr esp = (uptr)r;
    if (r->int_no < 32) {
        if (r->int_no == 3 && !(r->cs & 3)) {   /* kernel breakpoint: log and continue */
            klog("[idt] breakpoint at rip=%llx\n", r->rip);
            return esp;
        }
        /* A fault in a user program kills that program, not the system. */
        if ((r->cs & 3) && user_fault_hook) return user_fault_hook(r, exc_names[r->int_no]);
        panic_regs(exc_names[r->int_no], r);
    } else if (r->int_no < 48) {
        int irq = r->int_no - IRQ_BASE;
        /* Spurious IRQ7/IRQ15: check the in-service register. */
        if (irq == 7 || irq == 15) {
            u16 port = irq == 7 ? PIC1_CMD : PIC2_CMD;
            outb(port, 0x0B);
            if (!(inb(port) & 0x80)) {
                if (irq == 15) outb(PIC1_CMD, 0x20);
                return esp;
            }
        }
        if (irq_handlers[irq]) irq_handlers[irq](r);
        if (irq >= 8) outb(PIC2_CMD, 0x20);   /* EOI */
        outb(PIC1_CMD, 0x20);
        if (irq == 0 && sched_hook) esp = sched_hook(esp, 0);
    } else if (r->int_no == SYSCALL_YIELD) {
        if (sched_hook) esp = sched_hook(esp, 1);
    } else if (r->int_no == SYSCALL_VECTOR) {
        if (syscall_hook) esp = syscall_hook(r);
    }
    if ((((struct regs *)esp)->cs & 3) && user_return_hook) user_return_hook((struct regs *)esp);
    return esp;
}
