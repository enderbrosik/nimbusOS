/* NimbusOS - AArch64 MMU: the kernel's vmm.h interface on ARM page tables.
 *
 * 4 KiB granule, 39-bit addresses (TTBR0 only, three levels: 1 GiB, 2 MiB,
 * 4 KiB).  The first GiB is identity mapped with 2 MiB blocks: ARM-owned RAM
 * cached, the GPU's share (framebuffer) uncached, and on the Pi 3 the
 * peripherals as device memory.  The 3..4 GiB range holds the heap, DMA and
 * framebuffer windows from vmm.h (4 KiB pages) and, on the Pi 4, the
 * peripherals at 0xFC000000.  There are no user address spaces yet.
 * Reference: Arm ARM D5 "The AArch64 Virtual Memory System Architecture". */
#include "arch/arm64/pi.h"
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define D_VALID   1ull
#define D_TABLE   3ull                  /* table (levels 1-2) or page (level 3) */
#define D_BLOCK   1ull
#define D_ATTR(i) ((u64)(i) << 2)
#define D_ISH     (3ull << 8)
#define D_AF      (1ull << 10)
#define D_XN      ((1ull << 53) | (1ull << 54))
#define ATTR_DEV  0                     /* MAIR index: device nGnRnE */
#define ATTR_WB   1                     /* normal, write-back */
#define ATTR_NC   2                     /* normal, non-cacheable */
#define ADDR_MASK 0x0000FFFFFFFFF000ull

static u64 l1[512] __attribute__((aligned(4096)));
static u64 l2_low[512] __attribute__((aligned(4096)));
static u64 l2_high[512] __attribute__((aligned(4096)));

static u64 block(u64 pa, int attr)
{
    u64 d = pa | D_BLOCK | D_ATTR(attr) | D_AF;
    if (attr == ATTR_DEV) d |= D_XN; else d |= D_ISH;
    return d;
}

void mmu_init(void)
{
    for (u64 i = 0; i < 512; i++) {
        u64 pa = i << 21;
        int attr = pa < g_arm_mem ? ATTR_WB : ATTR_NC;
        if (g_pi_model == 3 && pa >= 0x3F000000) attr = ATTR_DEV;
        l2_low[i] = block(pa, attr);
    }
    l1[0] = (uptr)l2_low | D_TABLE;
    if (g_pi_model == 3) l1[1] = block(0x40000000, ATTR_DEV);          /* ARM local peripherals */
    else
        for (u64 pa = 0xFC000000; pa < 0x100000000ull; pa += 0x200000)
            l2_high[(pa - 0xC0000000) >> 21] = block(pa, ATTR_DEV);
    l1[3] = (uptr)l2_high | D_TABLE;

    u64 mair = (0x00ull << 0) | (0xFFull << 8) | (0x44ull << 16);
    u64 tcr = 25 | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (2ull << 32);
    SET_SYSREG(mair_el1, mair);
    SET_SYSREG(tcr_el1, tcr);
    SET_SYSREG(ttbr0_el1, (uptr)l1);
    __asm__ volatile("dsb ish\n\tisb\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
    u64 sctlr = SYSREG(sctlr_el1);
    sctlr |= (1 << 0) | (1 << 2) | (1 << 12);       /* MMU, data cache, instruction cache */
    sctlr &= ~(1ull << 1);                           /* no alignment checks */
    SET_SYSREG(sctlr_el1, sctlr);
    __asm__ volatile("isb" ::: "memory");
}

void vmm_init(u64 ram_top)
{
    kprintf("[vmm ] AArch64 MMU on: 4 KiB pages, first %u MiB identity mapped (%u MiB cached RAM), heap at %08x\n",
            1024, (u32)(g_arm_mem >> 20), VMM_HEAP_BASE);
}

static void flush_tlb(uptr virt)
{
    __asm__ volatile("dsb ishst\n\ttlbi vaae1is, %0\n\tdsb ish\n\tisb" :: "r"(virt >> 12) : "memory");
}

/* The level 3 entry for virt, creating its table if `create`. */
static u64 *pte(uptr virt, bool create)
{
    if (virt >> 39) return NULL;
    u64 *l1e = &l1[(virt >> 30) & 511];
    if ((*l1e & 3) != D_TABLE) return NULL;
    u64 *l2 = (u64 *)(uptr)(*l1e & ADDR_MASK);
    u64 *l2e = &l2[(virt >> 21) & 511];
    if (!(*l2e & D_VALID)) {
        if (!create) return NULL;
        u32 f = pmm_alloc();
        if (!f) return NULL;
        memset((void *)(uptr)f, 0, 4096);
        __asm__ volatile("dsb ishst" ::: "memory");
        *l2e = f | D_TABLE;
    } else if ((*l2e & 3) != D_TABLE) return NULL;                    /* a 2 MiB block */
    u64 *l3 = (u64 *)(uptr)(*l2e & ADDR_MASK);
    return &l3[(virt >> 12) & 511];
}

bool vmm_map(uptr virt, u64 phys, u32 flags)
{
    u64 *e = pte(virt & ~0xFFFull, true);
    if (!e) return false;
    int attr = (flags & PTE_PCD) ? ATTR_NC : ATTR_WB;
    *e = (phys & ADDR_MASK) | D_TABLE | D_ATTR(attr) | D_AF | D_ISH;
    flush_tlb(virt);
    return true;
}

u64 vmm_unmap(uptr virt)
{
    u64 *e = pte(virt & ~0xFFFull, false);
    if (!e || !(*e & D_VALID)) return 0;
    u64 pa = *e & ADDR_MASK;
    *e = 0;
    flush_tlb(virt);
    return pa;
}

u64 vmm_translate(uptr virt)
{
    if (virt < 0x40000000) return virt;
    u64 *e = pte(virt & ~0xFFFull, false);
    if (!e || !(*e & D_VALID)) return 0;
    return (*e & ADDR_MASK) | (virt & 0xFFF);
}

void vmm_map_range(uptr virt, u64 phys, u64 size, u32 flags)
{
    for (u64 o = 0; o < size; o += 4096) vmm_map(virt + o, phys + o, flags);
}

void vmm_identity_map(u64 phys, u64 size)
{
    if (phys + size <= 0x40000000) return;           /* already mapped */
    klog("[vmm ] identity map of %llx (+%llx) ignored\n", phys, size);
}

u64  vmm_space_create(void) { return 0; }
bool vmm_space_map(u64 space, uptr virt, u64 phys, u32 flags) { return false; }
u64  vmm_space_translate(u64 space, uptr virt) { return 0; }
void vmm_space_destroy(u64 space) {}
void vmm_space_switch(u64 space) {}
u64  vmm_space_current(void) { return 0; }
