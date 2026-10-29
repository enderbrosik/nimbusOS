/* NimbusOS JavaScript engine - fixed point numbers (see js_int.h). No floating point, no libc. */
#include "js/js_int.h"

typedef unsigned long long u64;

static js_num clampn(js_num v)
{
    if (v > JS_LIMIT) return JS_PINF;
    if (v < -JS_LIMIT) return JS_NINF;
    return v;
}

js_num num_from_int(long long i)
{
    if (i > JS_LIMIT / JS_SCALE) return JS_PINF;
    if (i < -(JS_LIMIT / JS_SCALE)) return JS_NINF;
    return i * JS_SCALE;
}

long long num_to_int64(js_num n)
{
    if (!num_isfin(n)) return 0;
    return n / JS_SCALE;
}

int num_to_int32(js_num n)
{
    if (!num_isfin(n)) return 0;
    long long v = n / JS_SCALE;
    return (int)(unsigned)((unsigned long long)v & 0xFFFFFFFFu);
}

js_num num_neg(js_num a)
{
    if (a == JS_NAN) return JS_NAN;
    if (a == JS_PINF) return JS_NINF;
    if (a == JS_NINF) return JS_PINF;
    return -a;
}

js_num num_add(js_num a, js_num b)
{
    if (a == JS_NAN || b == JS_NAN) return JS_NAN;
    if (num_isinf(a)) return (num_isinf(b) && a != b) ? JS_NAN : a;
    if (num_isinf(b)) return b;
    return clampn(a + b);
}

js_num num_sub(js_num a, js_num b) { return num_add(a, num_neg(b)); }

js_num num_mul(js_num a, js_num b)
{
    if (a == JS_NAN || b == JS_NAN) return JS_NAN;
    int neg = (a < 0) != (b < 0);
    if (num_isinf(a) || num_isinf(b)) {
        if (a == 0 || b == 0) return JS_NAN;
        return neg ? JS_NINF : JS_PINF;
    }
    u64 A = a < 0 ? (u64)-a : (u64)a, B = b < 0 ? (u64)-b : (u64)b;
    u64 ai = A / JS_SCALE, af = A % JS_SCALE, bi = B / JS_SCALE, bf = B % JS_SCALE;
    u64 t, r = 0;
    int ovf = 0;
    ovf |= __builtin_mul_overflow(ai, bi, &t);
    ovf |= __builtin_mul_overflow(t, (u64)JS_SCALE, &t);
    r = t;
    u64 x;
    ovf |= __builtin_mul_overflow(ai, bf, &x);
    ovf |= __builtin_add_overflow(r, x, &r);
    ovf |= __builtin_mul_overflow(af, bi, &x);
    ovf |= __builtin_add_overflow(r, x, &r);
    ovf |= __builtin_add_overflow(r, (af * bf + JS_SCALE / 2) / JS_SCALE, &r);
    if (ovf || r > (u64)JS_LIMIT) return neg ? JS_NINF : JS_PINF;
    return neg ? -(js_num)r : (js_num)r;
}

js_num num_div(js_num a, js_num b)
{
    if (a == JS_NAN || b == JS_NAN) return JS_NAN;
    int neg = (a < 0) != (b < 0);
    if (num_isinf(a)) return num_isinf(b) ? JS_NAN : (neg ? JS_NINF : JS_PINF);
    if (num_isinf(b)) return 0;
    if (b == 0) return a == 0 ? JS_NAN : (neg ? JS_NINF : JS_PINF);
    u64 A = a < 0 ? (u64)-a : (u64)a, B = b < 0 ? (u64)-b : (u64)b;
    u64 q = A / B, rem = A % B, frac = 0, r;
    for (int i = 0; i < 4; i++) {                 /* four more decimal digits, long division */
        rem *= 10;
        frac = frac * 10 + rem / B;
        rem %= B;
    }
    if (rem * 2 >= B) frac++;                      /* round half up */
    if (__builtin_mul_overflow(q, (u64)JS_SCALE, &r) || __builtin_add_overflow(r, frac, &r) || r > (u64)JS_LIMIT)
        return neg ? JS_NINF : JS_PINF;
    return neg ? -(js_num)r : (js_num)r;
}

js_num num_mod(js_num a, js_num b)
{
    if (a == JS_NAN || b == JS_NAN || num_isinf(a) || b == 0) return JS_NAN;
    if (num_isinf(b)) return a;
    return a % b;
}

int num_cmp(js_num a, js_num b)
{
    if (a == JS_NAN || b == JS_NAN) return 2;
    return a < b ? -1 : a > b ? 1 : 0;
}

js_num num_floor(js_num a)
{
    if (!num_isfin(a)) return a;
    long long v = a / JS_SCALE;
    if (a < 0 && a % JS_SCALE) v--;
    return v * JS_SCALE;
}

js_num num_ceil(js_num a)
{
    if (!num_isfin(a)) return a;
    long long v = a / JS_SCALE;
    if (a > 0 && a % JS_SCALE) v++;
    return v * JS_SCALE;
}

js_num num_trunc(js_num a) { return num_isfin(a) ? (a / JS_SCALE) * JS_SCALE : a; }

js_num num_round(js_num a)
{
    if (!num_isfin(a)) return a;
    return num_floor(a + JS_SCALE / 2);            /* JavaScript rounds .5 up, toward +Infinity */
}

js_num num_sqrt(js_num a)
{
    if (a == JS_NAN || a < 0) return JS_NAN;
    if (a == JS_PINF) return JS_PINF;
    if (a == 0) return 0;
    js_num y = a > JS_SCALE ? a / 2 : JS_SCALE;
    for (int i = 0; i < 64; i++) {
        js_num ny = num_div(num_add(y, num_div(a, y)), 2 * JS_SCALE);
        if (ny == y) break;
        y = ny;
    }
    return y;
}

js_num num_pow(js_num a, js_num b)
{
    if (b == 0) return JS_SCALE;                   /* x ** 0 is 1, even for NaN */
    if (a == JS_NAN || b == JS_NAN) return JS_NAN;
    if (a == JS_SCALE) return JS_NAN == b ? JS_NAN : (num_isinf(b) ? JS_NAN : JS_SCALE);
    if (num_isinf(b)) {
        js_num m = a < 0 ? num_neg(a) : a;
        if (m == JS_SCALE) return JS_NAN;
        if ((m > JS_SCALE) == (b == JS_PINF)) return JS_PINF;
        return 0;
    }
    if (b % JS_SCALE == 0) {
        long long n = b / JS_SCALE;
        int neg = n < 0;
        if (neg) n = -n;
        js_num r = JS_SCALE, base = a;
        while (n) {
            if (n & 1) r = num_mul(r, base);
            n >>= 1;
            if (n) base = num_mul(base, base);
            if (num_isinf(r) && n > 64) break;
        }
        return neg ? num_div(JS_SCALE, r) : r;
    }
    if (b == JS_SCALE / 2) return num_sqrt(a);
    return JS_NAN;                                 /* other fractional powers are not supported */
}

/* ---- text -------------------------------------------------------------------------------------------------- */

static int put_u64(u64 v, char *buf)
{
    char t[24]; int n = 0, len = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) buf[len++] = t[--n];
    return len;
}

int num_fmt(js_num n, char *buf)
{
    const char *s = 0;
    if (n == JS_NAN) s = "NaN"; else if (n == JS_PINF) s = "Infinity"; else if (n == JS_NINF) s = "-Infinity";
    if (s) { int i = 0; while (s[i]) { buf[i] = s[i]; i++; } buf[i] = 0; return i; }
    int len = 0;
    u64 A = n < 0 ? (u64)-n : (u64)n;
    if (n < 0) buf[len++] = '-';
    len += put_u64(A / JS_SCALE, buf + len);
    u64 f = A % JS_SCALE;
    if (f) {
        char d[4]; for (int i = 3; i >= 0; i--) { d[i] = (char)('0' + f % 10); f /= 10; }
        int last = 3; while (last >= 0 && d[last] == '0') last--;
        buf[len++] = '.';
        for (int i = 0; i <= last; i++) buf[len++] = d[i];
    }
    buf[len] = 0;
    return len;
}

int num_fmt_fixed(js_num n, int digits, char *buf)
{
    if (!num_isfin(n)) return num_fmt(n, buf);
    if (digits < 0) digits = 0;
    if (digits > 20) digits = 20;
    int len = 0;
    u64 A = n < 0 ? (u64)-n : (u64)n;
    if (digits < 4) {                              /* round half away from zero at `digits` */
        u64 unit = 1; for (int i = digits; i < 4; i++) unit *= 10;
        A = (A + unit / 2) / unit * unit;
    }
    if (n < 0 && A) buf[len++] = '-';
    len += put_u64(A / JS_SCALE, buf + len);
    if (digits > 0) {
        buf[len++] = '.';
        u64 f = A % JS_SCALE;
        char d[4]; for (int i = 3; i >= 0; i--) { d[i] = (char)('0' + f % 10); f /= 10; }
        for (int i = 0; i < digits; i++) buf[len++] = i < 4 ? d[i] : '0';
    }
    buf[len] = 0;
    return len;
}

int num_fmt_radix(js_num n, int radix, char *buf)
{
    if (radix < 2 || radix > 36 || radix == 10 || !num_isfin(n)) return num_fmt(n, buf);
    int len = 0;
    long long v = n / JS_SCALE;
    if (v < 0) { buf[len++] = '-'; v = -v; }
    char t[72]; int nt = 0;
    do { int d = (int)(v % radix); t[nt++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= radix; } while (v);
    while (nt) buf[len++] = t[--nt];
    buf[len] = 0;
    return len;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* Parses a decimal number at s (no sign). Returns characters used, 0 if there is none. */
static int parse_dec(const char *s, int len, js_num *out)
{
    int i = 0, nd = 0, pt = 0, seen_pt = 0, any = 0;
    unsigned char d[48];
    int inf_int = 0;
    while (i < len) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            any = 1;
            if (nd == 0 && c == '0') { if (seen_pt) pt--; }          /* leading zeros */
            else if (nd < 40) { d[nd++] = (unsigned char)(c - '0'); if (!seen_pt) pt++; }
            else if (!seen_pt) { pt++; }
            i++;
        } else if (c == '.' && !seen_pt) {
            if (!any && !(i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')) break;
            seen_pt = 1; i++;
        } else break;
    }
    if (!any) return 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        int j = i + 1, eneg = 0, ev = 0, ed = 0;
        if (j < len && (s[j] == '+' || s[j] == '-')) { eneg = s[j] == '-'; j++; }
        while (j < len && s[j] >= '0' && s[j] <= '9') { if (ev < 100000) ev = ev * 10 + (s[j] - '0'); j++; ed++; }
        if (ed) { pt += eneg ? -ev : ev; i = j; }
    }
    /* value = 0.d1d2d3... * 10^pt ; scaled by 10^4 means digits d[0 .. pt+3] form the integer */
    int want = pt + 4;
    js_num r = 0;
    if (nd == 0) { *out = 0; return i; }
    if (want < 0) { *out = 0; return i; }
    if (want > 24) { *out = JS_PINF; (void)inf_int; return i; }
    for (int k = 0; k < want; k++) {
        int dig = k < nd ? d[k] : 0;
        if (r > (JS_LIMIT - dig) / 10) { *out = JS_PINF; return i; }
        r = r * 10 + dig;
    }
    if (want < nd && d[want] >= 5) r++;
    *out = r > JS_LIMIT ? JS_PINF : r;
    return i;
}

js_num num_parse_prefix(const char *s, int len, int *used)
{
    int i = 0, neg = 0;
    while (i < len && is_ws(s[i])) i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    static const char inf[] = "Infinity";
    if (len - i >= 8 && memcmp(s + i, inf, 8) == 0) { *used = i + 8; return neg ? JS_NINF : JS_PINF; }
    js_num v;
    int n = parse_dec(s + i, len - i, &v);
    if (!n) { *used = 0; return JS_NAN; }
    *used = i + n;
    return neg ? num_neg(v) : v;
}

js_num num_parse(const char *s, int len)
{
    int i = 0, e = len;
    while (i < e && is_ws(s[i])) i++;
    while (e > i && is_ws(s[e - 1])) e--;
    if (i == e) return 0;
    if (e - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        js_num v = 0;
        for (int k = i + 2; k < e; k++) {
            int d;
            char c = s[k];
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return JS_NAN;
            v = v * 16 + d;
            if (v > JS_LIMIT / JS_SCALE) return JS_PINF;
        }
        return v * JS_SCALE;
    }
    int used;
    js_num v = num_parse_prefix(s + i, e - i, &used);
    return used == e - i ? v : JS_NAN;
}
