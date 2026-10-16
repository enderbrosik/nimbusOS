/*
 * NimbusOS - random bytes for TLS (keys, nonces).
 *
 * A SHA-256 pool stirred with the CPU timestamp counter at every packet
 * arrival (network timing jitter), the clock, and RDRAND when the CPU has
 * it.  Output blocks are hashed from the pool with a counter, and the pool
 * is re-keyed after every request, so earlier outputs can't be recovered
 * from later state.  Good enough for a hobby OS; not a certified DRBG.
 */
#include "net/tls.h"
#include "kernel/string.h"
#include "arch/x86/io.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"

static u8 pool[32];
static u64 counter;
static int have_rdrand = -1;

static u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
}

static bool rdrand64(u64 *v)
{
    if (have_rdrand < 0) {
        u32 a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        have_rdrand = (c >> 30) & 1;
    }
    if (!have_rdrand) return false;
    for (int i = 0; i < 10; i++) {
        u8 ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok) return true;
    }
    return false;
}

/* Mix a value into the pool (called from the network task per packet). */
void rng_stir(u64 v)
{
    struct sha256 s;
    u64 t = rdtsc();
    u32 f = irq_save();
    sha256_init(&s);
    sha256_update(&s, pool, 32);
    sha256_update(&s, &v, 8);
    sha256_update(&s, &t, 8);
    sha256_final(&s, pool);
    irq_restore(f);
}

void tls_random_bytes(u8 *buf, u32 n)
{
    struct rtc_time rt;
    rtc_read(&rt);
    u64 extra[6] = { rdtsc(), timer_ticks(), (u64)rt.second | (u64)rt.minute << 8 | (u64)rt.hour << 16 |
                     (u64)rt.day << 24 | (u64)rt.month << 32 | (u64)rt.year << 40, 0, 0, 0 };
    rdrand64(&extra[3]);
    rdrand64(&extra[4]);
    while (n) {
        u8 block[32];
        struct sha256 s;
        u32 f = irq_save();
        extra[5] = rdtsc();
        u64 c = counter++;
        sha256_init(&s);
        sha256_update(&s, "out", 3);
        sha256_update(&s, pool, 32);
        sha256_update(&s, &c, 8);
        sha256_update(&s, extra, sizeof extra);
        sha256_final(&s, block);
        sha256_init(&s);                       /* re-key the pool */
        sha256_update(&s, "next", 4);
        sha256_update(&s, pool, 32);
        sha256_update(&s, &c, 8);
        sha256_final(&s, pool);
        irq_restore(f);
        u32 take = n < 32 ? n : 32;
        memcpy(buf, block, take);
        buf += take; n -= take;
        memset(block, 0, 32);
    }
}
