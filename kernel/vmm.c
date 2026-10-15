/*
 * NimbusOS - paging (32-bit, two level, 4 KiB pages).
 *
 * Intel SDM Vol. 3A section 4.3 "32-bit Paging".  The last page directory
 * entry points at the page directory itself ("recursive mapping", OSDev wiki
 * "Page Tables#Recursive mapping"), so every page table is visible at
 * 0xFFC00000 + index * 4096 and the directory itself at 0xFFFFF000.  That lets
 * us edit page tables that live in any physical frame without needing a
 * separate physical memory window.
 */
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "kernel/panic.h"
#include "arch/x86/io.h"

#define PD_VIRT ((u32 *)0xFFFFF000)
#define PT_VIRT(i) ((u32 *)(0xFFC00000 + ((i) << 12)))

static u32 page_dir[1024] __attribute__((aligned(4096)));
static u32 low_tables[VMM_IDENTITY_END >> 22][1024] __attribute__((aligned(4096)));
static bool paging_on;

static inline void invlpg(u32 a) { __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory"); }

void vmm_init(void)
{
    memset(page_dir, 0, sizeof page_dir);
    for (u32 t = 0; t < (VMM_IDENTITY_END >> 22); t++) {
        for (u32 i = 0; i < 1024; i++) {
            u32 addr = (t << 22) | (i << 12);
            low_tables[t][i] = addr ? (addr | PTE_PRESENT | PTE_WRITE) : 0;   /* page 0 = NULL guard */
        }
        page_dir[t] = (u32)low_tables[t] | PTE_PRESENT | PTE_WRITE;
    }
    page_dir[1023] = (u32)page_dir | PTE_PRESENT | PTE_WRITE;
    __asm__ volatile("mov %0, %%cr3" :: "r"(page_dir));
    u32 cr0 = read_cr0();
    cr0 |= 0x80000000 | 0x00010000;   /* PG | WP */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    paging_on = true;
    kprintf("[vmm ] paging enabled, identity map 0-%uMiB, recursive PD at 0xFFFFF000\n",
            VMM_IDENTITY_END >> 20);
}

bool vmm_map(u32 virt, u32 phys, u32 flags)
{
    u32 f = irq_save();
    u32 pdi = virt >> 22, pti = (virt >> 12) & 1023;
    if (!(PD_VIRT[pdi] & PTE_PRESENT)) {
        u32 frame = pmm_alloc();
        if (!frame) { irq_restore(f); return false; }
        PD_VIRT[pdi] = frame | PTE_PRESENT | PTE_WRITE;
        invlpg((u32)PT_VIRT(pdi));
        memset(PT_VIRT(pdi), 0, 4096);
    }
    PT_VIRT(pdi)[pti] = (phys & ~0xFFFu) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(virt);
    irq_restore(f);
    return true;
}

u32 vmm_unmap(u32 virt)
{
    u32 f = irq_save();
    u32 pdi = virt >> 22, pti = (virt >> 12) & 1023;
    u32 phys = 0;
    if (PD_VIRT[pdi] & PTE_PRESENT) {
        phys = PT_VIRT(pdi)[pti] & ~0xFFFu;
        PT_VIRT(pdi)[pti] = 0;
        invlpg(virt);
    }
    irq_restore(f);
    return phys;
}

u32 vmm_translate(u32 virt)
{
    u32 pdi = virt >> 22, pti = (virt >> 12) & 1023;
    if (!(PD_VIRT[pdi] & PTE_PRESENT)) return 0;
    u32 pte = PT_VIRT(pdi)[pti];
    if (!(pte & PTE_PRESENT)) return 0;
    return (pte & ~0xFFFu) | (virt & 0xFFF);
}

void vmm_map_range(u32 virt, u32 phys, u32 size, u32 flags)
{
    u32 off = phys & 0xFFF;
    virt &= ~0xFFFu; phys &= ~0xFFFu;
    for (u32 a = 0; a < size + off; a += 4096)
        if (!vmm_map(virt + a, phys + a, flags)) panic("vmm_map_range: out of memory");
}

void vmm_identity_map(u32 phys, u32 size)
{
    u32 start = phys & ~0xFFFu, end = ALIGN_UP(phys + size, 4096);
    for (u32 a = start; a < end && a >= start; a += 4096)
        if (!vmm_translate(a) || a == 0) vmm_map(a, a, PTE_WRITE);
}
