/*
 * NimbusOS - paging (x86-64, four levels, 4 KiB and 2 MiB pages).
 *
 * Intel SDM Vol. 3A section 4.5 "4-Level Paging".  A virtual address is
 * split into PML4 / PDPT / PD / PT indices of 9 bits each plus a 12-bit
 * offset.  The kernel lives entirely in the low 4 GiB (PML4 entry 0):
 *
 *   - all usable RAM (up to 3 GiB) is identity mapped, with 2 MiB pages
 *     except for the first 2 MiB, which uses 4 KiB pages so that page 0
 *     stays unmapped as a NULL guard.  Because every physical frame the
 *     PMM hands out is identity mapped, page tables are edited directly
 *     through their physical address - no recursive mapping is needed;
 *   - 0xC0000000-0xFFFFFFFF holds the heap, DMA, MMIO and framebuffer
 *     windows (4 KiB pages, created on demand).
 *
 * User programs get their own PML4 ("address space") whose entry 0 points
 * at the kernel's shared PDPT (supervisor-only) and whose other entries map
 * the program at USER_BASE.  Switching CR3 switches programs.
 */
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "kernel/panic.h"
#include "arch/x86/io.h"

#define PTE_PS      0x080           /* 2 MiB page (in a PD entry) */
#define ADDR_MASK   0x000FFFFFFFFFF000ull
#define IDENT_MAX   0xC0000000ull   /* identity map at most 3 GiB of RAM */

static u64 kernel_pml4[512] __attribute__((aligned(4096)));
static u64 kernel_pdpt[512] __attribute__((aligned(4096)));
static u64 kernel_pd[4][512] __attribute__((aligned(4096)));
static u64 first_pt[512] __attribute__((aligned(4096)));
static u64 ident_top;
static u64 cur_space;               /* physical address of the loaded PML4 */

static inline void invlpg(uptr a) { __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory"); }
static inline u64 *table(u64 entry) { return (u64 *)(uptr)(entry & ADDR_MASK); }

void vmm_init(u64 ram_top)
{
    ident_top = ALIGN_UP(ram_top, 0x200000ull);
    if (ident_top > IDENT_MAX) ident_top = IDENT_MAX;
    if (ident_top < 0x2000000) ident_top = 0x2000000;      /* at least 32 MiB */
    memset(kernel_pml4, 0, sizeof kernel_pml4);
    memset(kernel_pdpt, 0, sizeof kernel_pdpt);
    memset(kernel_pd, 0, sizeof kernel_pd);
    for (int i = 0; i < 512; i++)
        first_pt[i] = i ? ((u64)i << 12) | PTE_PRESENT | PTE_WRITE : 0;   /* page 0 = NULL guard */
    for (int g = 0; g < 4; g++)
        kernel_pdpt[g] = (uptr)kernel_pd[g] | PTE_PRESENT | PTE_WRITE;
    kernel_pd[0][0] = (uptr)first_pt | PTE_PRESENT | PTE_WRITE;
    for (u64 a = 0x200000; a < ident_top; a += 0x200000)
        kernel_pd[a >> 30][(a >> 21) & 511] = a | PTE_PRESENT | PTE_WRITE | PTE_PS;
    kernel_pml4[0] = (uptr)kernel_pdpt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    cur_space = (uptr)kernel_pml4;
    write_cr3(cur_space);
    kprintf("[vmm ] 4-level paging, identity map 0-%uMiB (2 MiB pages), kernel windows at 0xC0000000\n",
            (u32)(ident_top >> 20));
}

/* Walk to the PTE for `virt` in the address space rooted at `pml4`,
 * creating missing tables when `create` is set.  NULL if absent or if the
 * address is covered by a 2 MiB page. */
static u64 *walk(u64 *pml4, uptr virt, bool create, bool user)
{
    u64 *t = pml4;
    for (int level = 3; level > 0; level--) {
        int idx = (virt >> (12 + 9 * level)) & 511;
        u64 e = t[idx];
        if (!(e & PTE_PRESENT)) {
            if (!create) return NULL;
            u32 frame = pmm_alloc();
            if (!frame) return NULL;
            memset((void *)(uptr)frame, 0, 4096);
            e = frame | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
            t[idx] = e;
        } else if (e & PTE_PS) {
            return NULL;
        } else if (user && !(e & PTE_USER) && level < 3) {
            t[idx] = e |= PTE_USER;
        }
        t = table(e);
    }
    return &t[(virt >> 12) & 511];
}

static bool map_in(u64 *pml4, uptr virt, u64 phys, u32 flags)
{
    u32 f = irq_save();
    u64 *pte = walk(pml4, virt, true, flags & PTE_USER);
    if (!pte) { irq_restore(f); return false; }
    *pte = (phys & ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    if ((uptr)pml4 == cur_space || virt < 0x100000000ull) invlpg(virt);
    irq_restore(f);
    return true;
}

bool vmm_map(uptr virt, u64 phys, u32 flags)
{
    if (virt >= 0x100000000ull) panic("vmm_map: kernel address %p above 4 GiB", (void *)virt);
    return map_in(kernel_pml4, virt, phys, flags & ~PTE_USER);
}

u64 vmm_unmap(uptr virt)
{
    u32 f = irq_save();
    u64 phys = 0;
    u64 *pte = walk(kernel_pml4, virt, false, false);
    if (pte && (*pte & PTE_PRESENT)) {
        phys = *pte & ADDR_MASK;
        *pte = 0;
        invlpg(virt);
    }
    irq_restore(f);
    return phys;
}

static u64 translate_in(u64 *pml4, uptr virt)
{
    u64 *t = pml4;
    for (int level = 3; level > 0; level--) {
        u64 e = t[(virt >> (12 + 9 * level)) & 511];
        if (!(e & PTE_PRESENT)) return 0;
        if (e & PTE_PS) {
            u64 size = 1ull << (12 + 9 * level);
            return (e & ADDR_MASK & ~(size - 1)) | (virt & (size - 1));
        }
        t = table(e);
    }
    u64 pte = t[(virt >> 12) & 511];
    if (!(pte & PTE_PRESENT)) return 0;
    return (pte & ADDR_MASK) | (virt & 0xFFF);
}

u64 vmm_translate(uptr virt) { return translate_in((u64 *)(uptr)cur_space, virt); }

void vmm_map_range(uptr virt, u64 phys, u64 size, u32 flags)
{
    u64 off = phys & 0xFFF;
    virt &= ~(uptr)0xFFF; phys &= ~0xFFFull;
    for (u64 a = 0; a < size + off; a += 4096)
        if (!vmm_map(virt + a, phys + a, flags)) panic("vmm_map_range: out of memory");
}

void vmm_identity_map(u64 phys, u64 size)
{
    u64 start = phys & ~0xFFFull, end = ALIGN_UP(phys + size, 4096);
    for (u64 a = start; a < end && a >= start; a += 4096)
        if (!vmm_translate(a) || a == 0) vmm_map(a, a, PTE_WRITE);
}

/* ---- user address spaces ------------------------------------------------ */

u64 vmm_space_create(void)
{
    u32 frame = pmm_alloc();
    if (!frame) return 0;
    u64 *pml4 = (u64 *)(uptr)frame;
    memset(pml4, 0, 4096);
    pml4[0] = kernel_pml4[0];       /* shared kernel low 4 GiB (supervisor pages) */
    return frame;
}

bool vmm_space_map(u64 space, uptr virt, u64 phys, u32 flags)
{
    if (virt < USER_BASE) return false;
    return map_in((u64 *)(uptr)space, virt, phys, flags | PTE_USER);
}

u64 vmm_space_translate(u64 space, uptr virt) { return translate_in((u64 *)(uptr)space, virt); }

/* Free every user page and table (PML4 entries 1..255), then the PML4. */
void vmm_space_destroy(u64 space)
{
    if (!space || space == (uptr)kernel_pml4) return;
    if (cur_space == space) vmm_space_switch(0);
    u64 *pml4 = (u64 *)(uptr)space;
    for (int i = 1; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        u64 *pdpt = table(pml4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            u64 *pd = table(pdpt[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                u64 *pt = table(pd[k]);
                for (int l = 0; l < 512; l++)
                    if (pt[l] & PTE_PRESENT) pmm_free((u32)(pt[l] & ADDR_MASK));
                pmm_free((u32)(uptr)pt);
            }
            pmm_free((u32)(uptr)pd);
        }
        pmm_free((u32)(uptr)pdpt);
    }
    pmm_free((u32)space);
}

void vmm_space_switch(u64 space)
{
    if (!space) space = (uptr)kernel_pml4;
    if (space == cur_space) return;
    cur_space = space;
    write_cr3(space);
}

u64 vmm_space_current(void) { return cur_space == (uptr)kernel_pml4 ? 0 : cur_space; }
