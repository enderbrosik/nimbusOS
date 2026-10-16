/*
 * NimbusOS - Intel ICH AC'97 audio controller (QEMU "-device AC97",
 * VirtualBox "ICH AC97").
 *
 * Two I/O BARs: NAM (codec mixer registers, AC'97 spec 2.3 section 5.7)
 * and NABM (bus master: Intel ICH "AC '97 Programmer's Reference",
 * section 2).  PCM output uses a ring of 32 buffer descriptors
 * {phys addr, sample count, flags}; the controller plays from CIV up to LVI.
 * We keep only a few buffers queued ahead of CIV (~50 ms) so sounds start
 * promptly, refilled by the sound mixer task; if the task falls behind and
 * the DMA engine halts at LVI it is simply restarted.
 */
#include "drivers/ac97.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "arch/x86/io.h"
#include "kernel/dma.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"

#define NBUF 32
#define LEAD 5                /* buffers queued ahead of the one playing */

/* NAM (mixer) */
#define NAM_RESET       0x00
#define NAM_MASTER      0x02
#define NAM_HEADPHONE   0x04
#define NAM_PCM_OUT     0x18
#define NAM_EXT_ID      0x28
#define NAM_EXT_CTRL    0x2A
#define NAM_FRONT_RATE  0x2C
/* NABM: PCM out box at 0x10 */
#define PO_BDBAR  0x10
#define PO_CIV    0x14
#define PO_LVI    0x15
#define PO_SR     0x16
#define PO_CR     0x1B
#define GLOB_CNT  0x2C
#define SR_DCH    0x01        /* DMA controller halted */
#define CR_RUN    0x01
#define CR_RESET  0x02

struct bd { u32 addr; u16 samples; u16 flags; } PACKED;

static u16 nam, nabm;
static bool present;
static u32 rate = 48000;
static struct bd *bdl;
static i16 *buf[NBUF];
static int fill;              /* next descriptor to fill */
static int queued_until;      /* LVI we last set */

static void delay_ms(u32 ms) { timer_busy_wait(ms); }

bool ac97_present(void) { return present; }
u32 ac97_rate(void) { return rate; }
u16 ac97_iobase(void) { return nabm; }

bool ac97_init(void)
{
    u8 bus, dev, fn;
    if (!pci_find(0x8086, 0x2415, &bus, &dev, &fn)) { kprintf("[snd ] no AC'97 controller\n"); return false; }
    nam = pci_read32(bus, dev, fn, 0x10) & ~1u;
    nabm = pci_read32(bus, dev, fn, 0x14) & ~1u;
    pci_write32(bus, dev, fn, 0x04, pci_read32(bus, dev, fn, 0x04) | 0x5);   /* I/O + bus master */

    outl(nabm + GLOB_CNT, 0x2);        /* leave cold reset */
    delay_ms(20);
    outw(nam + NAM_RESET, 0);          /* reset the codec registers */
    delay_ms(20);
    outw(nam + NAM_MASTER, 0x0000);    /* 0 dB, unmuted */
    outw(nam + NAM_HEADPHONE, 0x0000);
    outw(nam + NAM_PCM_OUT, 0x0808);   /* 0 dB gain */
    if (inw(nam + NAM_EXT_ID) & 1) {   /* variable rate audio */
        outw(nam + NAM_EXT_CTRL, inw(nam + NAM_EXT_CTRL) | 1);
        outw(nam + NAM_FRONT_RATE, 48000);
        rate = inw(nam + NAM_FRONT_RATE);
        if (!rate) rate = 48000;
    }

    u32 phys;
    bdl = dma_alloc_page(&phys);
    if (!bdl) return false;
    u32 bdl_phys = phys;
    for (int i = 0; i < NBUF; i += 2) {   /* two 2 KiB buffers per page */
        u8 *page = dma_alloc_page(&phys);
        if (!page) return false;
        buf[i] = (i16 *)page;
        buf[i + 1] = (i16 *)(page + 2048);
        bdl[i] = (struct bd){ phys, AC97_BUF_FRAMES * 2, 0 };
        bdl[i + 1] = (struct bd){ phys + 2048, AC97_BUF_FRAMES * 2, 0 };
    }
    outb(nabm + PO_CR, CR_RESET);
    for (int i = 0; i < 1000 && (inb(nabm + PO_CR) & CR_RESET); i++) io_wait();
    outl(nabm + PO_BDBAR, bdl_phys);
    /* prime LEAD buffers of silence and start */
    fill = LEAD;
    queued_until = LEAD - 1;
    outb(nabm + PO_LVI, queued_until);
    outb(nabm + PO_CR, CR_RUN);
    present = true;
    kprintf("[snd ] AC'97 at I/O %x/%x, %u Hz, 16-bit stereo\n", nam, nabm, rate);
    return true;
}

static int civ(void) { return inb(nabm + PO_CIV) & 31; }

i16 *ac97_next_buffer(void)
{
    if (!present) return NULL;
    int c = civ();
    int ahead = (fill - c + NBUF) % NBUF;          /* buffers queued after the playing one */
    if (ahead >= LEAD) return NULL;
    return buf[fill];
}

void ac97_submit(void)
{
    queued_until = fill;
    fill = (fill + 1) % NBUF;
    outb(nabm + PO_LVI, queued_until);
    u16 sr = inw(nabm + PO_SR);
    if (sr & SR_DCH) {                              /* ran dry and halted: restart */
        outw(nabm + PO_SR, 0x1C);                   /* clear status bits */
        outb(nabm + PO_CR, CR_RUN);
    }
}
