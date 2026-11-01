/* NimbusOS - boot log on the SD card.
 *
 * Every log line is kept in RAM and copied into NIMBUS.LOG on the card's
 * FAT32 boot partition: once the SD driver is up, then every two seconds,
 * and right away on a panic.  The file must already exist (the Pi build
 * ships a 512 KiB one); only its own data sectors are overwritten, so the
 * FAT and directory are never changed and a crash cannot damage the card.
 * Space after the end of the text is filled with blanks. */
#include "arch/arm64/pi.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/task.h"
#include "arch/x86/io.h"

#define LOG_MAX (512 * 1024)
#define MAX_CLUS 2048

static char logbuf[LOG_MAX];
static u32 logpos;                       /* bytes of text in logbuf */
static u32 flushed;                      /* bytes already on the card */
static bool ready;
static u32 file_bytes, spc, data_lba, nclus;
static u32 clus[MAX_CLUS];
static u8 sec[512] __attribute__((aligned(16)));

void sdlog_append(const char *s)
{
    while (*s && logpos < LOG_MAX - 64) logbuf[logpos++] = *s++;
}

static u32 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* Sector holding byte `off` of the file. */
static u32 file_sector(u32 off)
{
    u32 s = off / 512;
    return data_lba + (clus[s / spc] - 2) * spc + s % spc;
}

static bool find_file(void)
{
    if (sd_read(0, 1, sec)) return false;
    u32 part = 0;
    for (int i = 0; i < 4; i++) {
        u8 *e = sec + 0x1BE + i * 16;
        if (e[4] == 0x0B || e[4] == 0x0C) { part = rd32(e + 8); break; }
    }
    if (!part) { kprintf("[log ] no FAT32 partition on the SD card\n"); return false; }
    if (sd_read(part, 1, sec) || rd16(sec + 11) != 512) return false;
    spc = sec[13];
    u32 reserved = rd16(sec + 14), nfats = sec[16], fatsz = rd32(sec + 36), root = rd32(sec + 44);
    u32 fat_lba = part + reserved;
    data_lba = fat_lba + nfats * fatsz;
    if (!spc || !fatsz) return false;

    u32 cached = 0xFFFFFFFF;
    static u8 fat[512] __attribute__((aligned(16)));
#define NEXT(c) ({ u32 _s = fat_lba + (c) * 4 / 512; if (_s != cached && !sd_read(_s, 1, fat)) cached = _s; \
                   rd32(fat + (c) * 4 % 512) & 0x0FFFFFFF; })
    u32 first = 0;
    for (u32 c = root; c >= 2 && c < 0x0FFFFFF8 && !first; c = NEXT(c)) {
        for (u32 s = 0; s < spc && !first; s++) {
            if (sd_read(data_lba + (c - 2) * spc + s, 1, sec)) return false;
            for (int i = 0; i < 512; i += 32) {
                if (sec[i] == 0) break;
                if (!memcmp(sec + i, "NIMBUS  LOG", 11) && !(sec[i + 11] & 0x18)) {
                    first = rd16(sec + i + 20) << 16 | rd16(sec + i + 26);
                    file_bytes = rd32(sec + i + 28);
                    break;
                }
            }
        }
    }
    if (!first || file_bytes < 512) { kprintf("[log ] NIMBUS.LOG not found on the SD card (copy it there to keep a log)\n"); return false; }
    nclus = 0;
    for (u32 c = first; c >= 2 && c < 0x0FFFFFF8 && nclus < MAX_CLUS; c = NEXT(c)) clus[nclus++] = c;
    u32 cap = nclus * spc * 512;
    if (file_bytes > cap) file_bytes = cap;
    if (file_bytes > LOG_MAX) file_bytes = LOG_MAX;
    file_bytes &= ~511u;
    return true;
}

/* Write the sectors from `flushed` up to the end of the text (and blank the
 * rest of the file once, at the start). */
static void flush_range(u32 from, u32 to)
{
    for (u32 off = from & ~511u; off < to; off += 512) {
        for (int i = 0; i < 512; i++) {
            u32 p = off + i;
            sec[i] = p < logpos ? (u8)logbuf[p] : ' ';
        }
        if (off + 512 >= file_bytes) { sec[510] = '\r'; sec[511] = '\n'; }
        if (sd_write(file_sector(off), 1, sec)) { ready = false; return; }
    }
}

void sdlog_flush(void)
{
    if (!ready) return;
    u32 end = logpos < file_bytes ? logpos : file_bytes;
    if (end <= flushed && flushed) return;
    flush_range(flushed, end);
    flushed = end & ~511u;
}

void sdlog_init(void)
{
    if (!sd_init() || !find_file()) return;
    kprintf("[log ] writing this log to NIMBUS.LOG on the SD card (%u KiB)\n", file_bytes / 1024);
    u32 keep = logpos;
    logpos = 0;
    flush_range(0, file_bytes);              /* blank last boot's text */
    logpos = keep;
    ready = true;
    flushed = 0;
    sdlog_flush();
}

static void flush_task(void *arg)
{
    for (;;) {
        task_sleep(2000);
        u32 f = irq_save();
        sdlog_flush();
        irq_restore(f);
    }
}

void sdlog_start_task(void)
{
    if (ready) task_create("sdlog", flush_task, NULL);
}
