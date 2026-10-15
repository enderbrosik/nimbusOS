/*
 * NimbusOS - shutdown and restart.
 *
 * Shutdown uses ACPI S5 (soft-off): find the RSDP (in the EBDA or the BIOS
 * area 0xE0000-0xFFFFF), follow RSDT -> FADT ("FACP") for the PM1a/PM1b
 * control ports, and find the \_S5 package in the DSDT AML to learn the
 * SLP_TYP values; then write SLP_TYP | SLP_EN to PM1x_CNT.
 * References: ACPI 6.4 spec sections 5.2.5 (RSDP), 5.2.9 (FADT),
 * 7.4.2 (\_Sx states), 4.8.3.2 (PM1 control); OSDev wiki "Shutdown",
 * forum post "ACPI poweroff" (Kazinsal) for the AML package walk.
 *
 * Restart pulses the CPU reset line through the 8042 keyboard controller
 * (command 0xFE, OSDev wiki "Reboot"), falling back to a triple fault.
 */
#include "kernel/power.h"
#include "kernel/vmm.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "arch/x86/io.h"

static bool acpi_ok;
static u32 pm1a_cnt, pm1b_cnt, smi_cmd;
static u8 acpi_enable_val;
static u16 slp_typa, slp_typb;

static bool checksum(const u8 *p, u32 len)
{
    u8 s = 0;
    for (u32 i = 0; i < len; i++) s += p[i];
    return s == 0;
}

static const u8 *find_rsdp(void)
{
    /* The BIOS data area lives in page 0, which is unmapped (NULL guard):
     * map it briefly to read the EBDA segment. */
    vmm_map(0, 0, 0);
    u32 ebda = (u32)(*(volatile u16 *)0x40E) << 4;
    vmm_unmap(0);
    if (ebda >= 0x80000 && ebda < 0xA0000)
        for (u32 a = ebda; a < ebda + 1024; a += 16)
            if (!memcmp((void *)a, "RSD PTR ", 8) && checksum((u8 *)a, 20)) return (u8 *)a;
    for (u32 a = 0xE0000; a < 0x100000; a += 16)
        if (!memcmp((void *)a, "RSD PTR ", 8) && checksum((u8 *)a, 20)) return (u8 *)a;
    return NULL;
}

static const u8 *map_table(u32 phys)
{
    vmm_identity_map(phys, 36);
    u32 len = *(u32 *)(phys + 4);
    if (len < 36 || len > 0x100000) return NULL;
    vmm_identity_map(phys, len);
    if (!checksum((u8 *)phys, len)) return NULL;
    return (const u8 *)phys;
}

/* Parse "\_S5_" Package in AML: 08 '_S5_' 12 PkgLength NumElements
 * (0A byte | 00 | 01) ... */
static bool parse_s5(const u8 *dsdt)
{
    u32 len = *(u32 *)(dsdt + 4);
    for (u32 i = 36; i + 4 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4)) continue;
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))) continue;
        const u8 *p = dsdt + i + 4;
        if (*p != 0x12) continue;
        p++;
        p += ((*p & 0xC0) >> 6) + 1;          /* skip PkgLength */
        p++;                                   /* NumElements */
        if (*p == 0x0A) p++;
        slp_typa = *p++ << 10;
        if (*p == 0x0A) p++;
        slp_typb = *p << 10;
        return true;
    }
    return false;
}

void power_init(void)
{
    const u8 *rsdp = find_rsdp();
    if (!rsdp) { kprintf("[acpi] no RSDP found\n"); return; }
    const u8 *rsdt = map_table(*(u32 *)(rsdp + 16));
    if (!rsdt || memcmp(rsdt, "RSDT", 4)) { kprintf("[acpi] bad RSDT\n"); return; }
    u32 n = (*(u32 *)(rsdt + 4) - 36) / 4;
    const u8 *fadt = NULL;
    for (u32 i = 0; i < n; i++) {
        const u8 *t = map_table(*(u32 *)(rsdt + 36 + i * 4));
        if (t && !memcmp(t, "FACP", 4)) { fadt = t; break; }
    }
    if (!fadt) { kprintf("[acpi] no FADT\n"); return; }
    smi_cmd = *(u32 *)(fadt + 48);
    acpi_enable_val = fadt[52];
    pm1a_cnt = *(u32 *)(fadt + 64);
    pm1b_cnt = *(u32 *)(fadt + 68);
    const u8 *dsdt = map_table(*(u32 *)(fadt + 40));
    if (!dsdt || memcmp(dsdt, "DSDT", 4) || !parse_s5(dsdt)) { kprintf("[acpi] \\_S5 not found\n"); return; }
    acpi_ok = true;
    kprintf("[acpi] PM1a_CNT=%x PM1b_CNT=%x SLP_TYPa=%x SMI_CMD=%x\n", pm1a_cnt, pm1b_cnt, slp_typa >> 10, smi_cmd);
}

bool power_acpi_available(void) { return acpi_ok; }

/* Power-off takes effect asynchronously: give it ~100 ms (port 0x80 writes
 * take about 1 us each) before trying the next method. */
static void settle(void) { for (int i = 0; i < 100000; i++) io_wait(); }

void power_shutdown(void)
{
    kprintf("[acpi] shutting down\n");
    cli();
    if (acpi_ok) {
        if (smi_cmd && acpi_enable_val && !(inw(pm1a_cnt) & 1)) {   /* SCI_EN clear: enable ACPI mode */
            outb(smi_cmd, acpi_enable_val);
            for (int i = 0; i < 1000000 && !(inw(pm1a_cnt) & 1); i++) io_wait();
        }
        outw(pm1a_cnt, slp_typa | (1 << 13));
        if (pm1b_cnt) outw(pm1b_cnt, slp_typb | (1 << 13));
        settle();
    }
    /* emulator fallbacks: QEMU (PIIX4 PM base 0x600), Bochs/old QEMU, VirtualBox */
    outw(0x604, 0x2000);
    settle();
    outw(0xB004, 0x2000);
    settle();
    outw(0x4004, 0x3400);
    settle();
    kprintf("[acpi] shutdown failed; it is now safe to turn off your computer\n");
    for (;;) hlt();
}

void power_reboot(void)
{
    kprintf("[power] restarting\n");
    cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);                 /* pulse CPU reset line */
    for (volatile int i = 0; i < 1000000; i++) {}
    struct { u16 limit; u32 base; } PACKED null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(null_idt));   /* triple fault */
    for (;;) hlt();
}
