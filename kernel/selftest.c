/* NimbusOS - boot-time self tests for the core kernel (stage 2). */
#include "kernel/selftest.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/task.h"
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "kernel/panic.h"
#include "drivers/pit.h"

static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1103515245 + 12345; return rng >> 8; }

static void test_pmm(void)
{
    u32 before = pmm_free_frames();
    u32 a = pmm_alloc(), b = pmm_alloc();
    ASSERT(a && b && a != b && !(a & 0xFFF));
    pmm_free(a); pmm_free(b);
    ASSERT(pmm_free_frames() == before);
    kprintf("[test] pmm: alloc/free ok (%u frames free)\n", before);
}

static void test_vmm(void)
{
    u32 frame = pmm_alloc();
    u32 virt = 0xDEAD0000;
    ASSERT(vmm_map(virt, frame, PTE_WRITE));
    ASSERT(vmm_translate(virt + 0x123) == frame + 0x123);
    *(volatile u32 *)virt = 0xCAFEBABE;
    /* The identity map must show the same physical frame. */
    ASSERT(frame < VMM_IDENTITY_END ? *(volatile u32 *)frame == 0xCAFEBABE : 1);
    ASSERT(vmm_unmap(virt) == frame);
    ASSERT(vmm_translate(virt) == 0);
    pmm_free(frame);
    kprintf("[test] vmm: map/translate/unmap ok\n");
}

static void test_heap(void)
{
    enum { N = 200 };
    static u8 *ptrs[N];
    static u32 sizes[N];
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < N; i++) {
            if (ptrs[i] && (rnd() & 1)) {
                for (u32 j = 0; j < sizes[i]; j++) ASSERT(ptrs[i][j] == (u8)(i + j));
                kfree(ptrs[i]);
                ptrs[i] = NULL;
            }
            if (!ptrs[i]) {
                sizes[i] = (rnd() % 5 == 0) ? rnd() % 40000 : rnd() % 300;
                ptrs[i] = kmalloc(sizes[i]);
                ASSERT(ptrs[i]);
                for (u32 j = 0; j < sizes[i]; j++) ptrs[i][j] = (u8)(i + j);
            }
        }
        ASSERT(heap_check());
    }
    for (int i = 0; i < N; i++) { kfree(ptrs[i]); ptrs[i] = NULL; }
    ASSERT(heap_check());
    struct heap_stats st;
    heap_get_stats(&st);
    ASSERT(st.free_blocks == 1);   /* everything coalesced back */
    kprintf("[test] heap: %u allocs / %u frees, %u KiB mapped, fully coalesced\n",
            st.allocs, st.frees, st.mapped / 1024);
}

static volatile u32 counter_a, counter_b, spin_count;
static volatile bool stop_tasks;

static void task_a(void *arg)
{
    while (!stop_tasks) { counter_a++; klog("[task] A tick %u\n", counter_a); task_sleep(100); }
}
static void task_b(void *arg)
{
    while (!stop_tasks) { counter_b++; klog("[task] B tick %u\n", counter_b); task_sleep(150); }
}
static void task_spin(void *arg)
{
    /* Never yields: only preemption lets the others run. */
    while (!stop_tasks) spin_count++;
}

static void test_tasks(void)
{
    task_create("demo-A", task_a, NULL);
    task_create("demo-B", task_b, NULL);
    task_create("spinner", task_spin, NULL);
    u64 start = timer_ticks();
    while (timer_ticks() - start < 300) { /* busy: the kernel task does not yield either */ }
    stop_tasks = true;
    task_sleep(160);
    kprintf("[test] tasks: A ran %u times, B ran %u times, spinner %u loops in 0.3s\n",
            counter_a, counter_b, spin_count);
    ASSERT(counter_a >= 2 && counter_b >= 2 && spin_count > 1000);
    struct task_info ti[16];
    int n = task_list(ti, 16);
    for (int i = 0; i < n; i++) klog("[task] #%d %-10s state %d cpu %u%%\n", ti[i].id, ti[i].name, ti[i].state, ti[i].cpu_percent);
    kprintf("[test] scheduler: preemptive round robin ok\n");
}

void selftest_core(void)
{
    test_pmm();
    test_vmm();
    test_heap();
    test_tasks();
}
