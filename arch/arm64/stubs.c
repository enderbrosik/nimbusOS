/* NimbusOS - Raspberry Pi stand-ins for PC devices that are not supported
 * on the Pi yet (stage 1): keyboard and mouse (USB, next stage), IDE disks,
 * USB sticks, AC'97 sound, the e1000 network card, PCI, PS/2, the CMOS
 * clock and ring 3 programs. */
#include "kernel/types.h"
#include "kernel/string.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/ata.h"
#include "drivers/usb.h"
#include "drivers/ac97.h"
#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "drivers/ps2.h"
#include "drivers/rtc.h"
#include "drivers/pit.h"
#include "kernel/proc.h"
#include "arch/x86/idt.h"

uptr (*syscall_hook)(struct regs *r);
uptr (*user_fault_hook)(struct regs *r, const char *what);
void (*user_return_hook)(struct regs *r);

void keyboard_init(void) {}
bool keyboard_poll(struct key_event *ev) { return false; }
u8   keyboard_mods(void) { return 0; }

static int mw = 1024, mh = 768;
void mouse_init(void) {}
void mouse_set_bounds(int w, int h) { mw = w; mh = h; }
void mouse_set_speed(int quarters) {}
bool mouse_poll(struct mouse_event *ev) { return false; }
void mouse_get(int *x, int *y, u8 *buttons) { *x = mw / 2; *y = mh / 2; *buttons = 0; }

struct ata_drive g_ata[ATA_MAX_DRIVES];
void ata_init(void) {}
int  ata_read(int drive, u32 lba, u32 count, void *buf) { return -1; }
int  ata_write(int drive, u32 lba, u32 count, const void *buf) { return -1; }

bool usb_init(void) { return false; }
bool usb_disk_present(void) { return false; }
u32  usb_disk_sectors(void) { return 0; }
const char *usb_disk_name(void) { return ""; }
int  usb_disk_read(u32 lba, u32 count, void *buf) { return -1; }
int  usb_disk_write(u32 lba, u32 count, const void *buf) { return -1; }

bool ac97_init(void) { return false; }
bool ac97_present(void) { return false; }
u32  ac97_rate(void) { return 48000; }
u16  ac97_iobase(void) { return 0; }
i16 *ac97_next_buffer(void) { return NULL; }
void ac97_submit(void) {}

bool e1000_init(void) { return false; }
bool e1000_present(void) { return false; }
void e1000_mac(u8 out[6]) { memset(out, 0, 6); }
bool e1000_link_up(void) { return false; }
int  e1000_send(const void *frame, u32 len) { return -1; }
int  e1000_poll(void *buf, u32 max) { return 0; }

u32  pci_read32(u8 bus, u8 dev, u8 fn, u8 off) { return 0xFFFFFFFF; }
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {}
bool pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn) { return false; }
void pci_enumerate(void) {}

bool ps2_wait_write(void) { return false; }
bool ps2_wait_read(void) { return false; }
void ps2_write_cmd(u8 cmd) {}
void ps2_write_data(u8 data) {}
int  ps2_read_data(void) { return -1; }
void ps2_controller_init(void) {}

/* The Pi has no battery-backed clock: the time starts at the build time and
 * runs from the generic timer (until the network sets it, in a later stage). */
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

u32 rtc_unix_time(void) { return (u32)BUILD_EPOCH + (u32)(timer_ticks() / 1000); }
void rtc_read(struct rtc_time *t) { unix_to_rtc(rtc_unix_time(), t); }

void unix_to_rtc(u32 ts, struct rtc_time *o)
{
    u32 days = ts / 86400, rem = ts % 86400;
    o->hour = rem / 3600; o->minute = (rem % 3600) / 60; o->second = rem % 60;
    int y = 1970;
    while (days >= (u32)(leap(y) ? 366 : 365)) { days -= leap(y) ? 366 : 365; y++; }
    int m = 0;
    while (days >= (u32)(mdays[m] + (m == 1 && leap(y)))) { days -= mdays[m] + (m == 1 && leap(y)); m++; }
    o->year = y; o->month = m + 1; o->day = days + 1;
}

struct proc *proc_spawn(const u8 *elf, u32 len, int argc, char **argv, const char *cwd, const char **why)
{
    if (why) *why = "programs (.elf) are not supported on the Raspberry Pi yet";
    return NULL;
}
u32  proc_read_output(struct proc *p, char *buf, u32 max) { return 0; }
bool proc_wants_input(struct proc *p) { return false; }
void proc_send_input(struct proc *p, const char *line, u32 len) {}
bool proc_service(struct proc *p) { return false; }
bool proc_finished(struct proc *p) { return true; }
int  proc_exit_code(struct proc *p) { return -1; }
const char *proc_fault(struct proc *p) { return NULL; }
int  proc_pid(struct proc *p) { return 0; }
void proc_kill(struct proc *p) {}
void proc_release(struct proc *p) {}
int  proc_count(void) { return 0; }
void proc_init(void) {}
void syscall_init(void) {}
void proc_task_reaped(struct proc *p) {}
struct proc *proc_current(void) { return NULL; }
