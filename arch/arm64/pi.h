/* NimbusOS - Raspberry Pi platform: board detection, MMIO and the mailbox. */
#ifndef NIMBUS_PI_H
#define NIMBUS_PI_H
#include "kernel/types.h"

extern int  g_pi_model;          /* 3 (BCM2837, QEMU raspi3b) or 4 (BCM2711: Pi 4 / Pi 400) */
extern uptr g_periph;            /* main peripherals: 0x3F000000 or 0xFE000000 */
extern uptr g_local;             /* ARM local peripherals: 0x40000000 or 0xFF800000 */
extern u32  g_pi_revision;       /* board revision code (mailbox) */
extern u64  g_arm_mem;           /* bytes of RAM the ARM owns from address 0 */

static inline void mmio_w(uptr a, u32 v) { *(volatile u32 *)a = v; }
static inline u32  mmio_r(uptr a) { return *(volatile u32 *)a; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

#define SYSREG(name) ({ u64 _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })
#define SET_SYSREG(name, v) __asm__ volatile("msr " #name ", %0" :: "r"((u64)(v)) : "memory")

void pi_detect(void);
void pi_led(bool on);
void pi_led_blink(int times);
const char *pi_board_name(void);

/* Mailbox property interface (channel 8).  buf is a complete message,
 * 16-byte aligned; returns true when the firmware answered "success". */
bool mbox_call(volatile u32 *buf);
void dcache_clean_inval(const void *p, u64 len);

/* arch/arm64 pieces used by pi_main */
void uart_init(void);
void mmu_init(void);
bool fb_early(void);
void console_disable(void);
void console_write(const char *s);
void irq_init(void);
u64  irq_count(void);
void timer_early(void);
bool sd_init(void);
int  sd_read(u32 lba, u32 count, void *buf);
int  sd_write(u32 lba, u32 count, const void *buf);
void sdlog_init(void);
void sdlog_start_task(void);
void sdlog_append(const char *s);
void sdlog_flush(void);
#endif
