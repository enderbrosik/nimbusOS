/* NimbusOS - pseudo random numbers (Marsaglia xorshift128), seeded lazily
 * from the CPU time stamp counter and the RTC. Not for cryptography. */
#include "kernel/random.h"
#include "drivers/rtc.h"

static u32 st[4];
static bool seeded;

static void seed(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    st[0] = lo ^ 0x9E3779B9;
    st[1] = hi ^ rtc_unix_time();
    st[2] = lo * 2654435761u + 1;
    st[3] = 0xC0FFEE ^ (lo >> 7);
    seeded = true;
    for (int i = 0; i < 16; i++) rand32();
}

u32 rand32(void)
{
    if (!seeded) seed();
    u32 t = st[3], s = st[0];
    st[3] = st[2]; st[2] = st[1]; st[1] = s;
    t ^= t << 11;
    t ^= t >> 8;
    return st[0] = t ^ s ^ (s >> 19);
}

u32 rand_range(u32 n) { return n ? rand32() % n : 0; }
