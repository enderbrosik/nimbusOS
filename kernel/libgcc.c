/*
 * NimbusOS - the handful of compiler runtime helpers GCC/Clang emit calls to
 * on i386 for 64-bit division.  We do not link the toolchain's libgcc, so a
 * simple shift-and-subtract implementation lives here.
 */
#include "kernel/types.h"

static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}

u64 __udivdi3(u64 n, u64 d) { return udivmod64(n, d, NULL); }
u64 __umoddi3(u64 n, u64 d) { u64 r; udivmod64(n, d, &r); return r; }

i64 __divdi3(i64 n, i64 d)
{
    int neg = (n < 0) ^ (d < 0);
    u64 q = udivmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, NULL);
    return neg ? -(i64)q : (i64)q;
}

i64 __moddi3(i64 n, i64 d)
{
    u64 r;
    udivmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
    return n < 0 ? -(i64)r : (i64)r;
}

u64 __udivmoddi4(u64 n, u64 d, u64 *rem) { return udivmod64(n, d, rem); }
