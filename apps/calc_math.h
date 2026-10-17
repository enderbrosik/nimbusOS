/*
 * NimbusOS - Calculator arithmetic.
 *
 * Numbers are long long values scaled by CALC_SCALE (six decimal places) and
 * kept within +-CALC_LIMIT, so every intermediate result fits in 64 bits and
 * no wider type or libc is needed.  Used by apps/calculator.c and, on the
 * host, by tools/calcmath.c.
 */
#ifndef NIMBUS_CALC_MATH_H
#define NIMBUS_CALC_MATH_H

typedef long long calc_num;

#define CALC_DECIMALS 6
#define CALC_SCALE    1000000LL
#define CALC_LIMIT    100000000000000000LL          /* 1e17 scaled: 100 billion, six decimals */
#define CALC_MAX_INT  (CALC_LIMIT / CALC_SCALE)     /* 1e11: largest whole part */

enum { CALC_OK, CALC_DIV_ZERO, CALC_OVERFLOW };

static inline calc_num calc_abs(calc_num v) { return v < 0 ? -v : v; }

/* Parses "-12.5" (at most six decimals).  Returns 0 if the text is not a number in range. */
static inline int calc_parse(const char *s, calc_num *out)
{
    calc_num whole = 0, frac = 0;
    int neg = 0, seen = 0, dec = -1;                 /* dec: digits after the point, -1 if none */
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s == '.' && dec < 0) { dec = 0; continue; }
        if (*s < '0' || *s > '9') return 0;
        seen = 1;
        if (dec < 0) {
            if (whole > CALC_MAX_INT / 10) return 0;
            whole = whole * 10 + (*s - '0');
        } else {
            if (dec == CALC_DECIMALS) return 0;
            frac = frac * 10 + (*s - '0');
            dec++;
        }
    }
    if (!seen) return 0;
    for (int i = dec < 0 ? 0 : dec; i < CALC_DECIMALS; i++) frac *= 10;
    calc_num r = whole * CALC_SCALE + frac;
    if (r > CALC_LIMIT) return 0;
    *out = neg ? -r : r;
    return 1;
}

/* Writes v as plain decimal text with no trailing zeros.  Returns the length. */
static inline int calc_format(calc_num v, char *buf, int n)
{
    char digits[24], out[32];
    int nd = 0, len = 0;
    calc_num whole = calc_abs(v) / CALC_SCALE, frac = calc_abs(v) % CALC_SCALE;
    do { digits[nd++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    if (v < 0) out[len++] = '-';
    while (nd) out[len++] = digits[--nd];
    if (frac) {
        int last = CALC_DECIMALS;
        while (frac % 10 == 0) { frac /= 10; last--; }
        out[len++] = '.';
        for (int i = last - 1; i >= 0; i--) {
            calc_num p = 1;
            for (int k = 0; k < i; k++) p *= 10;
            out[len++] = (char)('0' + (frac / p) % 10);
        }
    }
    int i = 0;
    for (; i < len && i < n - 1; i++) buf[i] = out[i];
    if (n > 0) buf[i] = 0;
    return i;
}

/* a op b for op in + - * /, all operands and results scaled by CALC_SCALE. */
static inline int calc_apply(calc_num a, char op, calc_num b, calc_num *out)
{
    calc_num r = a;
    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': {
        /* (ia + fa/S)(ib + fb/S) * S = ia*ib*S + ia*fb + fa*ib + fa*fb/S */
        calc_num ia = a / CALC_SCALE, fa = a % CALC_SCALE;
        calc_num ib = b / CALC_SCALE, fb = b % CALC_SCALE;
        if (ia && ib && calc_abs(ia) > CALC_MAX_INT / calc_abs(ib)) return CALC_OVERFLOW;
        r = ia * ib * CALC_SCALE + ia * fb + fa * ib + fa * fb / CALC_SCALE;
        break;
    }
    case '/': {
        /* long division of the magnitudes: whole part, then six digits of the remainder */
        calc_num x = calc_abs(a), y = calc_abs(b), q, rem, frac = 0;
        if (y == 0) return CALC_DIV_ZERO;
        q = x / y;
        if (q > CALC_MAX_INT) return CALC_OVERFLOW;
        rem = x % y;
        for (int i = 0; i < CALC_DECIMALS; i++) {
            rem *= 10;
            frac = frac * 10 + rem / y;
            rem %= y;
        }
        r = q * CALC_SCALE + frac;
        if ((a < 0) != (b < 0)) r = -r;
        break;
    }
    }
    if (calc_abs(r) > CALC_LIMIT) return CALC_OVERFLOW;
    *out = r;
    return CALC_OK;
}

#endif
