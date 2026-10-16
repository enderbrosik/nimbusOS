/*
 * NimbusOS - Intel 8254x ("e1000") Gigabit Ethernet driver, as emulated by
 * QEMU (PCI 8086:100E, 82540EM) and VirtualBox.
 *
 * Intel "PCI/PCI-X Family of Gigabit Ethernet Controllers Software
 * Developer's Manual" (8254x SDM): section 13 register map, 3.2 receive and
 * 3.3 transmit descriptor rings, 14 initialisation; OSDev wiki "Intel
 * Ethernet i217".  Registers are memory-mapped through BAR0; the driver
 * polls (interrupts are masked) from the network task.
 */
#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "kernel/dma.h"
#include "kernel/vmm.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "arch/x86/io.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_ASDE  (1u << 5)
#define CTRL_SLU   (1u << 6)
#define CTRL_RST   (1u << 26)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define NDESC 32
#define BUFSZ 2048

struct rx_desc { u64 addr; u16 length; u16 csum; u8 status; u8 errors; u16 special; } PACKED;
struct tx_desc { u64 addr; u16 length; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } PACKED;

static volatile u8 *mmio;
static bool present;
static u8 mac[6];
static struct rx_desc *rx;
static struct tx_desc *tx;
static u8 *rx_buf[NDESC], *tx_buf[NDESC];
static int rx_cur, tx_cur;

static u32 rd(u32 r) { return *(volatile u32 *)(mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

bool e1000_present(void) { return present; }
void e1000_mac(u8 out[6]) { memcpy(out, mac, 6); }
bool e1000_link_up(void) { return present && (rd(REG_STATUS) & 2); }

static u16 eeprom_read(u8 addr)
{
    wr(REG_EERD, 1 | ((u32)addr << 8));
    for (int i = 0; i < 100000; i++) {
        u32 v = rd(REG_EERD);
        if (v & (1 << 4)) return v >> 16;
    }
    return 0;
}

/* allocate n 2 KiB buffers, two per DMA page */
static bool alloc_bufs(u8 **bufs, u64 *phys_out)
{
    for (int i = 0; i < NDESC; i += 2) {
        u32 p;
        u8 *page = dma_alloc_page(&p);
        if (!page) return false;
        bufs[i] = page; bufs[i + 1] = page + BUFSZ;
        phys_out[i] = p; phys_out[i + 1] = p + BUFSZ;
    }
    return true;
}

bool e1000_init(void)
{
    u8 bus, dev, fn;
    if (!pci_find(0x8086, 0x100E, &bus, &dev, &fn) && !pci_find(0x8086, 0x100F, &bus, &dev, &fn)) {
        kprintf("[net ] no e1000 network card\n");
        return false;
    }
    u32 bar = pci_read32(bus, dev, fn, 0x10) & ~0xFu;
    pci_write32(bus, dev, fn, 0x04, pci_read32(bus, dev, fn, 0x04) | 0x6);   /* memory + bus master */
    vmm_map_range(VMM_MMIO_BASE, bar, 128 * 1024, PTE_WRITE | PTE_PCD | PTE_PWT);
    mmio = (volatile u8 *)VMM_MMIO_BASE;

    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    timer_busy_wait(10);
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, (rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~CTRL_RST);
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    u32 ral = rd(REG_RAL), rah = rd(REG_RAH);
    if (ral || (rah & 0xFFFF)) {
        for (int i = 0; i < 4; i++) mac[i] = ral >> (i * 8);
        mac[4] = rah; mac[5] = rah >> 8;
    } else {
        for (int i = 0; i < 3; i++) { u16 w = eeprom_read(i); mac[i * 2] = w; mac[i * 2 + 1] = w >> 8; }
        wr(REG_RAL, mac[0] | mac[1] << 8 | mac[2] << 16 | (u32)mac[3] << 24);
        wr(REG_RAH, mac[4] | mac[5] << 8 | (1u << 31));
    }

    u32 p;
    u64 phys[NDESC];
    rx = dma_alloc_page(&p);
    if (!rx || !alloc_bufs(rx_buf, phys)) return false;
    for (int i = 0; i < NDESC; i++) { rx[i].addr = phys[i]; rx[i].status = 0; }
    wr(REG_RDBAL, p); wr(REG_RDBAH, 0);
    wr(REG_RDLEN, NDESC * sizeof(struct rx_desc));
    wr(REG_RDH, 0); wr(REG_RDT, NDESC - 1);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);      /* 2048-byte buffers */

    tx = dma_alloc_page(&p);
    if (!tx || !alloc_bufs(tx_buf, phys)) return false;
    for (int i = 0; i < NDESC; i++) { tx[i].addr = phys[i]; tx[i].status = 1; }   /* DD: free */
    wr(REG_TDBAL, p); wr(REG_TDBAH, 0);
    wr(REG_TDLEN, NDESC * sizeof(struct tx_desc));
    wr(REG_TDH, 0); wr(REG_TDT, 0);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);
    present = true;
    kprintf("[net ] e1000 MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], e1000_link_up() ? "up" : "down");
    return true;
}

int e1000_send(const void *frame, u32 len)
{
    if (!present || len > BUFSZ) return -1;
    u32 f = irq_save();
    struct tx_desc *d = &tx[tx_cur];
    for (int i = 0; i < 100000 && !(d->status & 1); i++) io_wait();
    if (!(d->status & 1)) { irq_restore(f); return -1; }
    memcpy(tx_buf[tx_cur], frame, len);
    if (len < 60) { memset(tx_buf[tx_cur] + len, 0, 60 - len); len = 60; }   /* minimum Ethernet frame */
    d->length = len;
    d->cmd = 0x01 | 0x02 | 0x08;      /* EOP | IFCS | RS */
    d->status = 0;
    tx_cur = (tx_cur + 1) % NDESC;
    wr(REG_TDT, tx_cur);
    irq_restore(f);
    return 0;
}

int e1000_poll(void *buf, u32 max)
{
    if (!present) return 0;
    u32 f = irq_save();
    struct rx_desc *d = &rx[rx_cur];
    if (!(d->status & 1)) { irq_restore(f); return 0; }
    u32 len = d->length;
    if (len > max) len = max;
    memcpy(buf, rx_buf[rx_cur], len);
    d->status = 0;
    wr(REG_RDT, rx_cur);
    rx_cur = (rx_cur + 1) % NDESC;
    irq_restore(f);
    return (int)len;
}
