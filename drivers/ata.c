/*
 * NimbusOS - ATA PIO driver (polling, LBA28).
 *
 * References: OSDev wiki "ATA PIO Mode"; ATA/ATAPI-6 (T13/1410D) sections
 * 8.15 IDENTIFY DEVICE, 8.27 READ SECTORS, 8.62 WRITE SECTORS, 8.9 FLUSH CACHE.
 *
 * Register block (offset from io base): 0 data, 1 error/features,
 * 2 sector count, 3 LBA 0-7, 4 LBA 8-15, 5 LBA 16-23, 6 drive/head,
 * 7 status/command.  Control block: alternate status / device control.
 * Status bits: 0x80 BSY, 0x40 DRDY, 0x20 DF, 0x08 DRQ, 0x01 ERR.
 */
#include "drivers/ata.h"
#include "arch/x86/io.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"

#define ST_BSY 0x80
#define ST_DF  0x20
#define ST_DRQ 0x08
#define ST_ERR 0x01

struct ata_drive g_ata[ATA_MAX_DRIVES];
static const char *names[4] = { "Primary Master", "Primary Slave", "Secondary Master", "Secondary Slave" };

/* Reading the alternate status 4 times gives the required 400 ns delay. */
static void delay400(struct ata_drive *d) { for (int i = 0; i < 4; i++) inb(d->ctrl); }

static int wait_not_busy(struct ata_drive *d)
{
    for (int i = 0; i < 1000000; i++) {
        u8 s = inb(d->io + 7);
        if (!(s & ST_BSY)) return s;
    }
    return -1;
}

static int wait_drq(struct ata_drive *d)
{
    for (int i = 0; i < 1000000; i++) {
        u8 s = inb(d->io + 7);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -1;
        if (s & ST_DRQ) return 0;
    }
    return -1;
}

static void identify(struct ata_drive *d)
{
    u16 id[256];
    outb(d->ctrl, 0x02);                         /* nIEN: we poll, no IRQs */
    outb(d->io + 6, d->slave ? 0xB0 : 0xA0);
    delay400(d);
    outb(d->io + 2, 0); outb(d->io + 3, 0); outb(d->io + 4, 0); outb(d->io + 5, 0);
    outb(d->io + 7, 0xEC);                       /* IDENTIFY DEVICE */
    u8 s = inb(d->io + 7);
    if (s == 0 || s == 0xFF) return;             /* no device / floating bus */
    if (wait_not_busy(d) < 0) return;
    if (inb(d->io + 4) || inb(d->io + 5)) return; /* ATAPI / SATA signature: not supported */
    if (wait_drq(d) < 0) return;
    insw(d->io, id, 256);
    for (int i = 0; i < 20; i++) {               /* model: words 27-46, byte swapped */
        d->model[i * 2] = id[27 + i] >> 8;
        d->model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
    d->sectors = id[60] | ((u32)id[61] << 16);
    d->present = d->sectors > 0;
}

void ata_init(void)
{
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        struct ata_drive *d = &g_ata[i];
        memset(d, 0, sizeof *d);
        d->index = i;
        d->io = i < 2 ? 0x1F0 : 0x170;
        d->ctrl = i < 2 ? 0x3F6 : 0x376;
        d->slave = i & 1;
        strcpy(d->name, names[i]);
        if (inb(d->io + 7) == 0xFF) continue;    /* no controller */
        identify(d);
        if (d->present)
            kprintf("[ata ] %s: \"%s\" %u sectors (%u MiB)\n", d->name, d->model, d->sectors, d->sectors / 2048);
    }
}

static int setup(struct ata_drive *d, u32 lba, u8 count, u8 cmd)
{
    if (wait_not_busy(d) < 0) return -1;
    outb(d->io + 6, 0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    delay400(d);
    outb(d->io + 2, count);
    outb(d->io + 3, lba & 0xFF);
    outb(d->io + 4, (lba >> 8) & 0xFF);
    outb(d->io + 5, (lba >> 16) & 0xFF);
    outb(d->io + 7, cmd);
    return 0;
}

int ata_read(int drive, u32 lba, u32 count, void *buf)
{
    if (drive < 0 || drive >= ATA_MAX_DRIVES || !g_ata[drive].present) return -1;
    struct ata_drive *d = &g_ata[drive];
    u8 *p = buf;
    u32 flags = irq_save();      /* keep a transfer atomic w.r.t. other tasks */
    while (count) {
        u32 n = count > 256 ? 256 : count;
        if (lba + n > d->sectors || setup(d, lba, n == 256 ? 0 : n, 0x20) < 0) goto fail;
        for (u32 i = 0; i < n; i++) {
            if (wait_drq(d) < 0) goto fail;
            insw(d->io, p, 256);
            p += 512;
        }
        lba += n; count -= n;
    }
    irq_restore(flags);
    return 0;
fail:
    irq_restore(flags);
    klog("[ata ] read error drive %d lba %u\n", drive, lba);
    return -1;
}

int ata_write(int drive, u32 lba, u32 count, const void *buf)
{
    if (drive < 0 || drive >= ATA_MAX_DRIVES || !g_ata[drive].present) return -1;
    struct ata_drive *d = &g_ata[drive];
    const u8 *p = buf;
    u32 flags = irq_save();
    while (count) {
        u32 n = count > 256 ? 256 : count;
        if (lba + n > d->sectors || setup(d, lba, n == 256 ? 0 : n, 0x30) < 0) goto fail;
        for (u32 i = 0; i < n; i++) {
            if (wait_drq(d) < 0) goto fail;
            outsw(d->io, p, 256);
            p += 512;
        }
        lba += n; count -= n;
    }
    outb(d->io + 7, 0xE7);       /* FLUSH CACHE */
    wait_not_busy(d);
    irq_restore(flags);
    return 0;
fail:
    irq_restore(flags);
    klog("[ata ] write error drive %d lba %u\n", drive, lba);
    return -1;
}
