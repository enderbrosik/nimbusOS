/*
 * NimbusOS - xHCI (USB 3) host controller driver and a USB mass-storage driver.
 *
 * Intel "eXtensible Host Controller Interface for Universal Serial Bus" spec rev 1.2
 * (register map ch. 5, rings and TRBs ch. 4 and 6, contexts ch. 6.2); USB Mass Storage
 * Class "Bulk-Only Transport" 1.0 and the SCSI commands INQUIRY, TEST UNIT READY,
 * READ CAPACITY(10), READ(10) and WRITE(10).
 *
 * The driver polls (no interrupts), handles one device directly on a root port (no hubs)
 * and moves at most 4 KiB per transfer through a bounce page.  Only the first mass-storage
 * device found is used.
 */
#include "drivers/usb.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "kernel/dma.h"
#include "kernel/vmm.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"

#define XHCI_MMIO (VMM_MMIO_BASE + 0x100000)
#define RING_TRBS 256

struct trb { u64 param; u32 status; u32 control; };
struct ring { struct trb *trb; u32 phys; int idx; u32 cycle; };

static volatile u8 *cap, *op, *rt, *db;
static int ports, csz;
static u64 *dcbaa;
static struct ring cmdr, ep0r, bulk_out, bulk_in;
static struct trb *evt;
static u32 evt_phys;
static int evt_idx;
static u32 evt_cycle = 1;
static u8 *dev_ctx, *in_ctx;
static u32 dev_ctx_phys, in_ctx_phys;
static int slot;
static u8 *bounce, *cbw_page;
static u32 bounce_phys, cbw_phys;
static u32 tag;
static int bulk_out_dci, bulk_in_dci;

static bool disk_ok;
static u32 disk_sectors;
static char disk_text[40];

static u32 r32(volatile u8 *b, u32 off) { return *(volatile u32 *)(b + off); }
static void w32(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }
static void w64(volatile u8 *b, u32 off, u64 v) { w32(b, off, (u32)v); w32(b, off + 4, (u32)(v >> 32)); }

/* ---------------------------------------------------------------- rings */
static bool ring_init(struct ring *r)
{
    r->trb = dma_alloc_page(&r->phys);
    if (!r->trb) return false;
    r->idx = 0;
    r->cycle = 1;
    r->trb[RING_TRBS - 1].param = r->phys;                       /* link back to the start */
    r->trb[RING_TRBS - 1].control = (6 << 10) | 2;               /* Link TRB, toggle cycle */
    return true;
}

/* Puts a TRB on the ring; returns its physical address. */
static u32 ring_push(struct ring *r, u64 param, u32 status, u32 control)
{
    struct trb *t = &r->trb[r->idx];
    t->param = param;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = control | r->cycle;
    u32 at = r->phys + r->idx * sizeof(struct trb);
    r->idx++;
    if (r->idx == RING_TRBS - 1) {
        r->trb[RING_TRBS - 1].control = (6 << 10) | 2 | r->cycle;
        r->cycle ^= 1;
        r->idx = 0;
    }
    return at;
}

/* ---------------------------------------------------------------- events */
struct ev { u32 type, code, slot, ep; u64 param; u32 residual; };

static bool next_event(struct ev *e, u32 timeout_ms)
{
    u64 end = timer_ticks() + timeout_ms;
    for (;;) {
        struct trb *t = &evt[evt_idx];
        if ((t->control & 1) == evt_cycle) {
            e->type = (t->control >> 10) & 63;
            e->code = t->status >> 24;
            e->residual = t->status & 0xFFFFFF;
            e->slot = t->control >> 24;
            e->ep = (t->control >> 16) & 31;
            e->param = t->param;
            evt_idx++;
            if (evt_idx == RING_TRBS) { evt_idx = 0; evt_cycle ^= 1; }
            w64(rt, 0x20 + 0x18, (u64)(evt_phys + evt_idx * sizeof(struct trb)) | 8);   /* ERDP, clear EHB */
            return true;
        }
        if (timer_ticks() >= end) return false;
    }
}

/* Waits for the completion of the command at cmd_phys; returns the completion code (1 = success), 0 on timeout. */
static u32 command(u64 param, u32 control, u32 *slot_out)
{
    u32 at = ring_push(&cmdr, param, 0, control);
    w32(db, 0, 0);
    struct ev e;
    u64 end = timer_ticks() + 2000;
    while (timer_ticks() < end && next_event(&e, 100)) {
        if (e.type == 33 && (u32)e.param == at) { if (slot_out) *slot_out = e.slot; return e.code; }
    }
    return 0;
}

/* Waits for a transfer event on the slot's endpoint dci; returns the completion code (0 = timeout). */
static u32 wait_transfer(int dci, u32 timeout_ms, u32 *residual)
{
    struct ev e;
    u64 end = timer_ticks() + timeout_ms;
    while (timer_ticks() < end && next_event(&e, 50)) {
        if (e.type == 32 && (int)e.ep == dci) { if (residual) *residual = e.residual; return e.code; }
    }
    return 0;
}

/* ---------------------------------------------------------------- control transfers */
static bool control(u8 type, u8 req, u16 value, u16 index, u16 len, void *data_phys_buf, u32 data_phys, bool in)
{
    (void)data_phys_buf;
    u64 setup = type | (u64)req << 8 | (u64)value << 16 | (u64)index << 32 | (u64)len << 48;
    u32 trt = len ? (in ? 3 : 2) : 0;
    ring_push(&ep0r, setup, 8, (2 << 10) | (1 << 6) | (trt << 16));
    if (len) ring_push(&ep0r, data_phys, len, (3 << 10) | (in ? 1 << 16 : 0));
    ring_push(&ep0r, 0, 0, (4 << 10) | (1 << 5) | ((len && in) ? 0 : 1 << 16));
    w32(db, slot * 4, 1);
    u32 code = wait_transfer(1, 2000, NULL);
    return code == 1 || code == 13;
}

static u8 *ctx_at(u8 *base, int n) { return base + n * (csz ? 64 : 32); }

/* ---------------------------------------------------------------- bring-up of one device */
static bool setup_device(int port)
{
    u32 sc = r32(op, 0x400 + port * 0x10);
    int speed = (sc >> 10) & 15;
    u32 code = 0, s = 0;
    code = command(0, (9 << 10), &s);                                       /* Enable Slot */
    if (code != 1 || !s) return false;
    slot = s;
    dev_ctx = dma_alloc_page(&dev_ctx_phys);
    in_ctx = dma_alloc_page(&in_ctx_phys);
    if (!dev_ctx || !in_ctx || !ring_init(&ep0r)) return false;
    dcbaa[slot] = dev_ctx_phys;
    u32 *ic = (u32 *)ctx_at(in_ctx, 0);
    ic[1] = 3;                                                              /* add slot and EP0 contexts */
    u32 *sl = (u32 *)ctx_at(in_ctx, 1);
    sl[0] = ((u32)speed << 20) | (1u << 27);
    sl[1] = (u32)(port + 1) << 16;
    u32 mps = speed == 4 ? 512 : speed == 2 ? 8 : 64;
    u32 *e0 = (u32 *)ctx_at(in_ctx, 2);
    e0[1] = (4 << 3) | (3 << 1) | (mps << 16);
    *(u64 *)&e0[2] = (u64)ep0r.phys | 1;
    e0[4] = 8;
    code = command(in_ctx_phys, (11 << 10) | ((u32)slot << 24), NULL);      /* Address Device */
    if (code != 1) { kprintf("[usb ] address device failed (code %u)\n", code); return false; }
    return true;
}

static int g_cfg = 1;

/* Reads descriptors, finds a mass-storage (bulk-only) interface and configures its endpoints. */
static bool setup_msc(void)
{
    u8 *buf = bounce;
    if (!control(0x80, 6, 0x0100, 0, 18, buf, bounce_phys, true)) return false;      /* device descriptor */
    if (!control(0x80, 6, 0x0200, 0, 9, buf, bounce_phys, true)) return false;       /* config header: total length */
    u32 total = buf[2] | buf[3] << 8;
    if (total > 1024) total = 1024;
    g_cfg = buf[5];
    if (!control(0x80, 6, 0x0200, 0, total, buf, bounce_phys, true)) return false;
    int out_ep = 0, in_ep = 0, out_mps = 0, in_mps = 0;
    bool msc = false;
    for (u32 i = 0; i + 2 <= total && buf[i]; i += buf[i]) {
        if (buf[i + 1] == 4) msc = buf[i + 5] == 8 && buf[i + 6] == 6 && buf[i + 7] == 0x50 && !out_ep;
        else if (buf[i + 1] == 5 && msc && (buf[i + 3] & 3) == 2) {
            int addr = buf[i + 2], mps = buf[i + 4] | buf[i + 5] << 8;
            if (addr & 0x80) { if (!in_ep) { in_ep = addr & 15; in_mps = mps; } }
            else if (!out_ep) { out_ep = addr; out_mps = mps; }
        }
    }
    if (!out_ep || !in_ep) return false;
    if (!control(0x00, 9, g_cfg, 0, 0, NULL, 0, false)) return false;                 /* Set Configuration */
    bulk_out_dci = out_ep * 2;
    bulk_in_dci = in_ep * 2 + 1;
    if (!ring_init(&bulk_out) || !ring_init(&bulk_in)) return false;
    int maxdci = bulk_out_dci > bulk_in_dci ? bulk_out_dci : bulk_in_dci;
    memset(in_ctx, 0, 4096);
    u32 *ic = (u32 *)ctx_at(in_ctx, 0);
    ic[1] = 1 | (1u << bulk_out_dci) | (1u << bulk_in_dci);
    u32 *sl = (u32 *)ctx_at(in_ctx, 1);
    memcpy(sl, ctx_at(dev_ctx, 0), 32);                                                /* current slot context */
    sl[0] = (sl[0] & 0x07FFFFFF) | ((u32)maxdci << 27);
    u32 *eo = (u32 *)ctx_at(in_ctx, bulk_out_dci + 1);
    eo[1] = (2 << 3) | (3 << 1) | ((u32)out_mps << 16);
    *(u64 *)&eo[2] = (u64)bulk_out.phys | 1;
    eo[4] = 512;
    u32 *ei = (u32 *)ctx_at(in_ctx, bulk_in_dci + 1);
    ei[1] = (6 << 3) | (3 << 1) | ((u32)in_mps << 16);
    *(u64 *)&ei[2] = (u64)bulk_in.phys | 1;
    ei[4] = 512;
    u32 code = command(in_ctx_phys, (12 << 10) | ((u32)slot << 24), NULL);            /* Configure Endpoint */
    if (code != 1) { kprintf("[usb ] configure endpoints failed (code %u)\n", code); return false; }
    return true;
}

/* ---------------------------------------------------------------- bulk-only transport */
static bool bulk(struct ring *r, int dci, u32 phys, u32 len)
{
    ring_push(r, phys, len, (1 << 10) | (1 << 5));
    w32(db, slot * 4, dci);
    u32 code = wait_transfer(dci, 5000, NULL);
    return code == 1 || code == 13;
}

/* One SCSI command: data is the bounce page (len bytes, in or out). */
static bool scsi(const u8 *cb, int cblen, u32 len, bool in)
{
    u8 *c = cbw_page;
    memset(c, 0, 64);
    *(u32 *)(c + 0) = 0x43425355;
    *(u32 *)(c + 4) = ++tag;
    *(u32 *)(c + 8) = len;
    c[12] = in ? 0x80 : 0;
    c[14] = cblen;
    memcpy(c + 15, cb, cblen);
    if (!bulk(&bulk_out, bulk_out_dci, cbw_phys, 31)) return false;
    if (len && !bulk(in ? &bulk_in : &bulk_out, in ? bulk_in_dci : bulk_out_dci, bounce_phys, len)) return false;
    if (!bulk(&bulk_in, bulk_in_dci, cbw_phys + 64, 13)) return false;
    u8 *s = cbw_page + 64;
    return *(u32 *)s == 0x53425355 && *(u32 *)(s + 4) == tag && s[12] == 0;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static bool probe_disk(void)
{
    u8 cb[16];
    memset(cb, 0, sizeof cb);
    cb[0] = 0x12; cb[4] = 36;                                                   /* INQUIRY */
    if (!scsi(cb, 6, 36, true)) return false;
    char vp[32];
    memcpy(vp, bounce + 8, 8);
    vp[8] = ' ';
    memcpy(vp + 9, bounce + 16, 16);
    vp[25] = 0;
    int n = 25;
    while (n > 0 && vp[n - 1] == ' ') vp[--n] = 0;
    strlcpy(disk_text, vp, sizeof disk_text);
    bool ready = false;
    for (int i = 0; i < 10 && !ready; i++) {
        memset(cb, 0, sizeof cb);
        ready = scsi(cb, 6, 0, false);                                          /* TEST UNIT READY */
        if (!ready) timer_busy_wait(100);
    }
    if (!ready) return false;
    memset(cb, 0, sizeof cb);
    cb[0] = 0x25;                                                               /* READ CAPACITY(10) */
    if (!scsi(cb, 10, 8, true)) return false;
    u32 last = be32(bounce), bs = be32(bounce + 4);
    if (bs != 512) { kprintf("[usb ] block size %u is not supported\n", bs); return false; }
    disk_sectors = last + 1;
    return true;
}

static int rw(u32 lba, u32 count, void *buf, bool write)
{
    u8 *p = buf;
    while (count) {
        u32 n = count > 8 ? 8 : count;
        u8 cb[16];
        memset(cb, 0, sizeof cb);
        cb[0] = write ? 0x2A : 0x28;
        cb[2] = lba >> 24; cb[3] = lba >> 16; cb[4] = lba >> 8; cb[5] = lba;
        cb[7] = n >> 8; cb[8] = n;
        if (write) memcpy(bounce, p, n * 512);
        if (!scsi(cb, 10, n * 512, !write)) return -1;
        if (!write) memcpy(p, bounce, n * 512);
        p += n * 512; lba += n; count -= n;
    }
    return 0;
}

bool usb_disk_present(void) { return disk_ok; }
u32 usb_disk_sectors(void) { return disk_sectors; }
const char *usb_disk_name(void) { return disk_text; }
int usb_disk_read(u32 lba, u32 count, void *buf) { return disk_ok && lba + count <= disk_sectors ? rw(lba, count, buf, false) : -1; }
int usb_disk_write(u32 lba, u32 count, const void *buf) { return disk_ok && lba + count <= disk_sectors ? rw(lba, count, (void *)buf, true) : -1; }

/* ---------------------------------------------------------------- controller start-up */
bool usb_init(void)
{
    u8 bus = 0, dev = 0, fn = 0;
    bool found = false;
    for (int d = 0; d < 32 && !found; d++)
        for (int f = 0; f < 8 && !found; f++) {
            u32 id = pci_read32(0, d, f, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
            if ((pci_read32(0, d, f, 8) >> 8) == 0x0C0330) { bus = 0; dev = d; fn = f; found = true; }
        }
    if (!found) return false;
    u32 bar0 = pci_read32(bus, dev, fn, 0x10);
    u64 bar = bar0 & ~0xFull;
    if (((bar0 >> 1) & 3) == 2) bar |= (u64)pci_read32(bus, dev, fn, 0x14) << 32;
    pci_write32(bus, dev, fn, 0x04, pci_read32(bus, dev, fn, 0x04) | 0x6);
    vmm_map_range(XHCI_MMIO, bar, 64 * 1024, PTE_WRITE | PTE_PCD | PTE_PWT);
    cap = (volatile u8 *)XHCI_MMIO;
    op = cap + (cap[0]);
    u32 hcs1 = r32(cap, 4), hcs2 = r32(cap, 8), hcc = r32(cap, 0x10);
    ports = hcs1 >> 24;
    int slots = hcs1 & 0xFF;
    csz = (hcc >> 2) & 1;
    db = cap + (r32(cap, 0x14) & ~3u);
    rt = cap + (r32(cap, 0x18) & ~0x1Fu);

    w32(op, 0, r32(op, 0) & ~1u);                                               /* stop, then reset */
    for (int i = 0; i < 200 && !(r32(op, 4) & 1); i++) timer_busy_wait(1);
    w32(op, 0, r32(op, 0) | 2);
    for (int i = 0; i < 1000 && ((r32(op, 0) & 2) || (r32(op, 4) & (1 << 11))); i++) timer_busy_wait(1);
    if (r32(op, 4) & (1 << 11)) { kprintf("[usb ] xHCI controller would not reset\n"); return false; }

    u32 p;
    dcbaa = dma_alloc_page(&p);
    u32 dcbaa_phys = p;
    u32 nscratch = ((hcs2 >> 21) & 31) << 5 | ((hcs2 >> 27) & 31);
    if (nscratch) {
        u64 *arr = dma_alloc_page(&p);
        if (!arr || nscratch > 500) return false;
        for (u32 i = 0; i < nscratch; i++) { u32 sp; if (!dma_alloc_page(&sp)) return false; arr[i] = sp; }
        dcbaa[0] = p;
    }
    if (!dcbaa || !ring_init(&cmdr)) return false;
    evt = dma_alloc_page(&evt_phys);
    u64 *erst = dma_alloc_page(&p);
    bounce = dma_alloc_page(&bounce_phys);
    cbw_page = dma_alloc_page(&cbw_phys);
    if (!evt || !erst || !bounce || !cbw_page) return false;
    erst[0] = evt_phys;
    erst[1] = RING_TRBS;
    w32(op, 0x38, slots > 255 ? 255 : slots);                                   /* CONFIG: max device slots */
    w64(op, 0x30, dcbaa_phys);
    w64(op, 0x18, (u64)cmdr.phys | 1);                                          /* command ring + cycle bit */
    w32(rt, 0x20 + 0x08, 1);                                                    /* ERSTSZ */
    w64(rt, 0x20 + 0x18, evt_phys);                                             /* ERDP */
    w64(rt, 0x20 + 0x10, p);                                                    /* ERSTBA */
    w32(op, 0, r32(op, 0) | 1);                                                 /* run */
    for (int i = 0; i < 200 && (r32(op, 4) & 1); i++) timer_busy_wait(1);

    for (int port = 0; port < ports && !disk_ok; port++) {
        u32 sc = r32(op, 0x400 + port * 0x10);
        if (!(sc & 1)) continue;                                                // nothing connected
        const u32 keep = ~((1u << 1) | (0x7Fu << 17));                          // never write the change bits back
        if (!(sc & 2)) {                                                        // USB 2 ports need a reset
            w32(op, 0x400 + port * 0x10, (sc & keep) | (1u << 4));
            for (int i = 0; i < 500 && !(r32(op, 0x400 + port * 0x10) & (1u << 21)); i++) timer_busy_wait(1);
            w32(op, 0x400 + port * 0x10, (r32(op, 0x400 + port * 0x10) & keep) | (1u << 21));
        }
        sc = r32(op, 0x400 + port * 0x10);
        if (!(sc & 2)) continue;
        timer_busy_wait(20);
        if (!setup_device(port) || !setup_msc()) continue;
        if (!probe_disk()) continue;
        disk_ok = true;
    }
    if (disk_ok) kprintf("[usb ] USB disk \"%s\", %u MiB\n", disk_text, disk_sectors / 2048);
    else kprintf("[usb ] xHCI controller, %d ports, no USB disk\n", ports);
    return disk_ok;
}
