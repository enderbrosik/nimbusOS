/*
 * NimbusOS - kernel slots in the boot area, for NimbusUpdate.
 *
 * The boot area (LBA 1..4095, before the NimbusFS partition) holds stage 2
 * and two kernel slots: A at LBA 64 and B at LBA 2080, 2016 sectors each.
 * The MBR's boot-info block says which one stage 2 loads (kernel LBA at
 * offset 0x1AC, sector count at 0x1B0).  An update writes the new kernel to
 * the slot that is *not* booted, reads it back and checks its SHA-256, and
 * only then rewrites the MBR - one sector - to switch slots.  If anything
 * fails before that last write, the system still boots the old kernel.
 */
#include "kernel/bootarea.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "drivers/ata.h"
#include "fs/vfs.h"
#include "net/crypto.h"

#define CHUNK 64        /* sectors per ATA transfer */

static int boot_drive(const char **why)
{
    if (vfs_booted_from_ramdisk()) { *why = "running from the live CD; install NimbusOS to a disk first"; return -1; }
    if (!g_root || g_root->drive < 0 || !g_ata[g_root->drive].present) { *why = "boot disk not found"; return -1; }
    return g_root->drive;
}

static int read_mbr(int drive, u8 mbr[512], const char **why)
{
    if (ata_read(drive, 0, 1, mbr)) { *why = "cannot read the MBR"; return -1; }
    if (memcmp(mbr + 0x1A0, "NBT1", 4) || mbr[510] != 0x55 || mbr[511] != 0xAA) { *why = "MBR has no NimbusOS boot info"; return -1; }
    return 0;
}

int bootarea_info(struct bootarea_info *bi, const char **why)
{
    static u8 mbr[512];
    int d = boot_drive(why);
    if (d < 0 || read_mbr(d, mbr, why)) return -1;
    bi->kernel_lba = *(u32 *)(mbr + 0x1AC);
    bi->kernel_sectors = *(u32 *)(mbr + 0x1B0);
    bi->active = bi->kernel_lba == KSLOT_B_LBA ? 'B' : 'A';
    return 0;
}

int bootarea_active_hash(u32 len, u8 hash[32])
{
    const char *why;
    struct bootarea_info bi;
    if (bootarea_info(&bi, &why) || len > bi.kernel_sectors * 512) return -1;
    u8 *buf = kmalloc(CHUNK * 512);
    if (!buf) return -1;
    struct sha256 s;
    sha256_init(&s);
    int d = g_root->drive;
    for (u32 done = 0; done < len;) {
        u32 n = MIN((u32)CHUNK, (len - done + 511) / 512);
        if (ata_read(d, bi.kernel_lba + done / 512, n, buf)) { kfree(buf); return -1; }
        u32 take = MIN(n * 512, len - done);
        sha256_update(&s, buf, take);
        done += take;
    }
    sha256_final(&s, hash);
    kfree(buf);
    return 0;
}

int bootarea_install_kernel(const u8 *image, u32 len, const char **why)
{
    static u8 mbr[512], check[512];
    int d = boot_drive(why);
    if (d < 0 || read_mbr(d, mbr, why)) return -1;
    u32 sectors = (len + 511) / 512;
    if (!len || sectors > KSLOT_SECTORS) { *why = "kernel image does not fit a slot"; return -1; }
    u32 cur = *(u32 *)(mbr + 0x1AC);
    u32 target = cur == KSLOT_B_LBA ? KSLOT_A_LBA : KSLOT_B_LBA;
    u8 *buf = kzalloc(CHUNK * 512);
    if (!buf) { *why = "out of memory"; return -1; }
    /* 1. write the inactive slot */
    for (u32 s = 0; s < sectors; s += CHUNK) {
        u32 n = MIN((u32)CHUNK, sectors - s);
        u32 off = s * 512, take = MIN(n * 512, len - off);
        memset(buf, 0, CHUNK * 512);
        memcpy(buf, image + off, take);
        if (ata_write(d, target + s, n, buf)) { kfree(buf); *why = "disk write error"; return -1; }
    }
    /* 2. read it back and compare hashes */
    u8 want[32], got[32];
    sha256(image, len, want);
    struct sha256 st;
    sha256_init(&st);
    for (u32 s = 0; s < sectors; s += CHUNK) {
        u32 n = MIN((u32)CHUNK, sectors - s);
        if (ata_read(d, target + s, n, buf)) { kfree(buf); *why = "disk read error"; return -1; }
        sha256_update(&st, buf, MIN(n * 512, len - s * 512));
    }
    sha256_final(&st, got);
    kfree(buf);
    if (memcmp(want, got, 32)) { *why = "kernel slot verification failed"; return -1; }
    /* 3. switch slots: one sector */
    *(u32 *)(mbr + 0x1AC) = target;
    *(u32 *)(mbr + 0x1B0) = sectors;
    if (ata_write(d, 0, 1, mbr) || ata_read(d, 0, 1, check) || memcmp(mbr, check, 512)) {
        *why = "writing the MBR failed";
        return -1;
    }
    kprintf("[boot] kernel %u bytes written to slot %c (LBA %u), now active\n", len,
            target == KSLOT_B_LBA ? 'B' : 'A', target);
    return 0;
}
