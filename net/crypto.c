/*
 * NimbusOS - cryptography for TLS 1.3 (see net/crypto.h for the list and
 * the specifications each part follows).  No dependencies beyond the
 * fixed-width types, so the same file runs in the kernel and on the host.
 */
#include "net/crypto.h"

static void cpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void zero(void *d, size_t n) { volatile u8 *a = d; while (n--) *a++ = 0; }

int crypto_memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    u8 d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put_be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put_le32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }
static u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
static void put_be64(u8 *p, u64 v) { put_be32(p, (u32)(v >> 32)); put_be32(p + 4, (u32)v); }
static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static u32 rotl(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

/* ======================================================================
 * SHA-256 (FIPS 180-4)
 * ==================================================================== */
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(struct sha256 *s, const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_init(struct sha256 *s)
{
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    cpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t len)
{
    const u8 *p = data;
    s->len += len;
    while (len) {
        u32 take = 64 - s->n;
        if (take > len) take = (u32)len;
        cpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) { sha256_block(s, s->buf); s->n = 0; }
    }
}

void sha256_final(struct sha256 *s, u8 out[32])
{
    u64 bits = s->len * 8;
    u8 pad = 0x80;
    sha256_update(s, &pad, 1);
    u8 z = 0;
    while (s->n != 56) sha256_update(s, &z, 1);
    u8 l[8];
    put_be64(l, bits);
    sha256_update(s, l, 8);
    for (int i = 0; i < 8; i++) put_be32(out + 4 * i, s->h[i]);
}

void sha256(const void *data, size_t len, u8 out[32])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

/* ---- HMAC (RFC 2104) and HKDF (RFC 5869) ---- */
void hmac_sha256(const u8 *key, size_t klen, const void *data, size_t len, u8 out[32])
{
    u8 k[64], pad[64], inner[32];
    zero(k, 64);
    if (klen > 64) sha256(key, klen, k); else cpy(k, key, klen);
    struct sha256 s;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, data, len); sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, inner, 32); sha256_final(&s, out);
    zero(k, 64);
}

void hkdf_extract(const u8 *salt, size_t slen, const u8 *ikm, size_t ilen, u8 prk[32])
{
    u8 zeros[32];
    zero(zeros, 32);
    if (!salt || !slen) { salt = zeros; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const u8 prk[32], const u8 *info, size_t ilen, u8 *out, size_t olen)
{
    u8 t[32 + 256 + 1], block[32];
    size_t tlen = 0;
    for (u8 i = 1; olen; i++) {
        size_t n = 0;
        if (i > 1) { cpy(t, block, 32); n = 32; }
        if (ilen > 256) ilen = 256;
        cpy(t + n, info, ilen); n += ilen;
        t[n++] = i;
        hmac_sha256(prk, 32, t, n, block);
        size_t take = olen < 32 ? olen : 32;
        cpy(out + tlen, block, take);
        tlen += take; olen -= take;
    }
}

void hkdf_expand_label(const u8 secret[32], const char *label, const u8 *ctx, size_t clen, u8 *out, size_t olen)
{
    u8 info[2 + 1 + 6 + 64 + 1 + 64];
    size_t ll = 0;
    while (label[ll]) ll++;
    size_t n = 0;
    info[n++] = (u8)(olen >> 8);
    info[n++] = (u8)olen;
    info[n++] = (u8)(6 + ll);
    cpy(info + n, "tls13 ", 6); n += 6;
    cpy(info + n, label, ll); n += ll;
    info[n++] = (u8)clen;
    cpy(info + n, ctx, clen); n += clen;
    hkdf_expand(secret, info, n, out, olen);
}

/* ======================================================================
 * X25519 (RFC 7748): field elements in radix 2^51, five 64-bit limbs.
 * ==================================================================== */
typedef u64 fe[5];
typedef unsigned __int128 u128;
#define M51 ((1ull << 51) - 1)

static void fe_carry(fe h)
{
    for (int k = 0; k < 2; k++) {
        u64 c;
        c = h[0] >> 51; h[0] &= M51; h[1] += c;
        c = h[1] >> 51; h[1] &= M51; h[2] += c;
        c = h[2] >> 51; h[2] &= M51; h[3] += c;
        c = h[3] >> 51; h[3] &= M51; h[4] += c;
        c = h[4] >> 51; h[4] &= M51; h[0] += 19 * c;
    }
}

static void fe_frombytes(fe h, const u8 s[32])
{
    u64 t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = 0;
        for (int j = 7; j >= 0; j--) t[i] = (t[i] << 8) | s[8 * i + j];
    }
    h[0] = t[0] & M51;
    h[1] = ((t[0] >> 51) | (t[1] << 13)) & M51;
    h[2] = ((t[1] >> 38) | (t[2] << 26)) & M51;
    h[3] = ((t[2] >> 25) | (t[3] << 39)) & M51;
    h[4] = (t[3] >> 12) & M51;          /* drops the top bit, as RFC 7748 requires */
}

static void fe_tobytes(u8 s[32], const fe in)
{
    fe h;
    cpy(h, in, sizeof h);
    fe_carry(h);
    /* subtract p if h >= p */
    u64 q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;
    h[0] += 19 * q;
    u64 c;
    c = h[0] >> 51; h[0] &= M51; h[1] += c;
    c = h[1] >> 51; h[1] &= M51; h[2] += c;
    c = h[2] >> 51; h[2] &= M51; h[3] += c;
    c = h[3] >> 51; h[3] &= M51; h[4] += c;
    h[4] &= M51;
    u64 t[4];
    t[0] = h[0] | (h[1] << 51);
    t[1] = (h[1] >> 13) | (h[2] << 38);
    t[2] = (h[2] >> 26) | (h[3] << 25);
    t[3] = (h[3] >> 39) | (h[4] << 12);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) s[8 * i + j] = (u8)(t[i] >> (8 * j));
}

static void fe_add(fe h, const fe a, const fe b) { for (int i = 0; i < 5; i++) h[i] = a[i] + b[i]; fe_carry(h); }

static void fe_sub(fe h, const fe a, const fe b)
{
    /* add 2p so the result stays positive */
    h[0] = a[0] + 0xFFFFFFFFFFFDAull - b[0];
    for (int i = 1; i < 5; i++) h[i] = a[i] + 0xFFFFFFFFFFFFEull - b[i];
    fe_carry(h);
}

static void fe_mul(fe h, const fe a, const fe b)
{
    u64 b1 = 19 * b[1], b2 = 19 * b[2], b3 = 19 * b[3], b4 = 19 * b[4];
    u128 r0 = (u128)a[0] * b[0] + (u128)a[1] * b4 + (u128)a[2] * b3 + (u128)a[3] * b2 + (u128)a[4] * b1;
    u128 r1 = (u128)a[0] * b[1] + (u128)a[1] * b[0] + (u128)a[2] * b4 + (u128)a[3] * b3 + (u128)a[4] * b2;
    u128 r2 = (u128)a[0] * b[2] + (u128)a[1] * b[1] + (u128)a[2] * b[0] + (u128)a[3] * b4 + (u128)a[4] * b3;
    u128 r3 = (u128)a[0] * b[3] + (u128)a[1] * b[2] + (u128)a[2] * b[1] + (u128)a[3] * b[0] + (u128)a[4] * b4;
    u128 r4 = (u128)a[0] * b[4] + (u128)a[1] * b[3] + (u128)a[2] * b[2] + (u128)a[3] * b[1] + (u128)a[4] * b[0];
    u64 c;
    c = (u64)(r0 >> 51); h[0] = (u64)r0 & M51; r1 += c;
    c = (u64)(r1 >> 51); h[1] = (u64)r1 & M51; r2 += c;
    c = (u64)(r2 >> 51); h[2] = (u64)r2 & M51; r3 += c;
    c = (u64)(r3 >> 51); h[3] = (u64)r3 & M51; r4 += c;
    c = (u64)(r4 >> 51); h[4] = (u64)r4 & M51;
    h[0] += 19 * c;
    c = h[0] >> 51; h[0] &= M51; h[1] += c;
}

static void fe_sq(fe h, const fe a) { fe_mul(h, a, a); }

static void fe_mul_small(fe h, const fe a, u64 k)
{
    u128 c = 0;
    for (int i = 0; i < 5; i++) {
        c += (u128)a[i] * k;
        h[i] = (u64)c & M51;
        c >>= 51;
    }
    h[0] += 19 * (u64)c;
    fe_carry(h);
}

static void fe_invert(fe out, const fe z)
{
    /* z^(p-2) by square-and-multiply over the bits of p-2 = 2^255 - 21 */
    fe r, t;
    cpy(t, z, sizeof t);
    r[0] = 1; r[1] = r[2] = r[3] = r[4] = 0;
    /* p-2 in binary: 250 ones, then 01011 (bits 4..0) */
    for (int i = 254; i >= 0; i--) {
        fe_sq(r, r);
        int bit = i >= 5 ? 1 : (0x0B >> i) & 1;
        if (bit) fe_mul(r, r, t);
    }
    cpy(out, r, sizeof r);
}

static void fe_cswap(fe a, fe b, u64 swap)
{
    u64 mask = 0 - swap;
    for (int i = 0; i < 5; i++) {
        u64 x = mask & (a[i] ^ b[i]);
        a[i] ^= x; b[i] ^= x;
    }
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32])
{
    u8 k[32];
    cpy(k, scalar, 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    fe x1, x2, z2, x3, z3, A, AA, B, BB, E, C, D, DA, CB, t;
    fe_frombytes(x1, point);
    x2[0] = 1; x2[1] = x2[2] = x2[3] = x2[4] = 0;
    zero(z2, sizeof z2);
    cpy(x3, x1, sizeof x3);
    z3[0] = 1; z3[1] = z3[2] = z3[3] = z3[4] = 0;
    u64 swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        u64 bit = (k[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(A, x2, z2);  fe_sq(AA, A);
        fe_sub(B, x2, z2);  fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);  fe_sub(D, x3, z3);
        fe_mul(DA, D, A);   fe_mul(CB, C, B);
        fe_add(t, DA, CB);  fe_sq(x3, t);
        fe_sub(t, DA, CB);  fe_sq(t, t);  fe_mul(z3, x1, t);
        fe_mul(x2, AA, BB);
        fe_mul_small(t, E, 121665); fe_add(t, AA, t); fe_mul(z2, E, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    zero(k, 32);
}

void x25519_base(u8 out[32], const u8 scalar[32])
{
    u8 nine[32];
    zero(nine, 32);
    nine[0] = 9;
    x25519(out, scalar, nine);
}

/* ======================================================================
 * ChaCha20 and Poly1305 (RFC 8439)
 * ==================================================================== */
#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8);  c += d; b ^= c; b = rotl(b, 7)

static void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
    u32 s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    cpy(x, s, sizeof s);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
}

static void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], const u8 *in, u8 *out, size_t len)
{
    u8 ks[64];
    while (len) {
        chacha20_block(key, counter++, nonce, ks);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
}

struct poly1305 { u32 r[5], h[5], pad[4]; u8 buf[16]; u32 n; };

static void poly_init(struct poly1305 *p, const u8 key[32])
{
    p->r[0] = (le32(key + 0)) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
    p->n = 0;
}

static void poly_blocks(struct poly1305 *p, const u8 *m, size_t len, u32 hibit)
{
    u32 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (len >= 16) {
        h0 += (le32(m + 0)) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
        u32 c;
        c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff; d1 += c;
        c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff; d2 += c;
        c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff; d3 += c;
        c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff; d4 += c;
        c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        m += 16; len -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(struct poly1305 *p, const u8 *m, size_t len)
{
    if (p->n) {
        while (p->n < 16 && len) { p->buf[p->n++] = *m++; len--; }
        if (p->n < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->n = 0;
    }
    size_t full = len & ~(size_t)15;
    poly_blocks(p, m, full, 1u << 24);
    m += full; len -= full;
    while (len--) p->buf[p->n++] = *m++;
}

/* RFC 8439 pads each AEAD section to 16 bytes with zeros */
static void poly_pad16(struct poly1305 *p)
{
    static const u8 z[16];
    if (p->n) poly_update(p, z, 16 - p->n);
}

static void poly_final(struct poly1305 *p, u8 tag[16])
{
    if (p->n) {
        p->buf[p->n++] = 1;
        while (p->n < 16) p->buf[p->n++] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    /* g = h - p; use g if h >= p */
    u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    u32 g4 = h4 + c - (1u << 26);
    u32 mask = (g4 >> 31) - 1;             /* all ones if h >= p */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);
    /* h = h % 2^128, then + pad */
    u32 w0 = h0 | (h1 << 26);
    u32 w1 = (h1 >> 6) | (h2 << 20);
    u32 w2 = (h2 >> 12) | (h3 << 14);
    u32 w3 = (h3 >> 18) | (h4 << 8);
    u64 f;
    f = (u64)w0 + p->pad[0];             put_le32(tag + 0, (u32)f);
    f = (u64)w1 + p->pad[1] + (f >> 32); put_le32(tag + 4, (u32)f);
    f = (u64)w2 + p->pad[2] + (f >> 32); put_le32(tag + 8, (u32)f);
    f = (u64)w3 + p->pad[3] + (f >> 32); put_le32(tag + 12, (u32)f);
}

static void chacha_poly_tag(const u8 key[32], const u8 nonce[12], const u8 *ad, size_t adlen,
                            const u8 *ct, size_t len, u8 tag[16])
{
    u8 block[64], lens[16];
    chacha20_block(key, 0, nonce, block);
    struct poly1305 p;
    poly_init(&p, block);
    poly_update(&p, ad, adlen); poly_pad16(&p);
    poly_update(&p, ct, len);   poly_pad16(&p);
    put_le64(lens, adlen);
    put_le64(lens + 8, len);
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    zero(block, 64);
}

void chacha20_poly1305_seal(const u8 key[32], const u8 nonce[12], const u8 *ad, size_t adlen,
                            const u8 *pt, size_t len, u8 *out)
{
    chacha20_xor(key, 1, nonce, pt, out, len);
    chacha_poly_tag(key, nonce, ad, adlen, out, len, out + len);
}

int chacha20_poly1305_open(const u8 key[32], const u8 nonce[12], const u8 *ad, size_t adlen,
                           const u8 *in, size_t len_with_tag, u8 *pt)
{
    if (len_with_tag < 16) return -1;
    size_t len = len_with_tag - 16;
    u8 tag[16];
    chacha_poly_tag(key, nonce, ad, adlen, in, len, tag);
    if (crypto_memcmp(tag, in + len, 16)) return -1;
    chacha20_xor(key, 1, nonce, in, pt, len);
    return 0;
}

/* ======================================================================
 * AES-128 (FIPS 197) and GCM (NIST SP 800-38D)
 * ==================================================================== */
static u8 sbox[256];
static int sbox_ready;

static u8 rotl8(u8 x, int n) { return (u8)((x << n) | (x >> (8 - n))); }

/* Build the S-box from its definition (multiplicative inverse in GF(2^8)
 * followed by the affine transform) instead of typing the table in. */
static void make_sbox(void)
{
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));     /* p *= 3 */
        q ^= (u8)(q << 1); q ^= (u8)(q << 2); q ^= (u8)(q << 4);   /* q /= 3 */
        if (q & 0x80) q ^= 0x09;
        u8 x = q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4);
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    sbox_ready = 1;
}

static u32 sub_word(u32 w)
{
    return (u32)sbox[w >> 24] << 24 | (u32)sbox[(w >> 16) & 255] << 16 | (u32)sbox[(w >> 8) & 255] << 8 | sbox[w & 255];
}

void aes128_init(struct aes128 *a, const u8 key[16])
{
    if (!sbox_ready) make_sbox();
    static const u8 rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36 };
    for (int i = 0; i < 4; i++) a->rk[i] = be32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        u32 t = a->rk[i - 1];
        if (i % 4 == 0) t = sub_word(rotl(t, 8)) ^ ((u32)rcon[i / 4 - 1] << 24);
        a->rk[i] = a->rk[i - 4] ^ t;
    }
}

static u8 xtime(u8 x) { return (u8)((x << 1) ^ (x & 0x80 ? 0x1B : 0)); }

void aes128_encrypt(const struct aes128 *a, const u8 in[16], u8 out[16])
{
    u8 s[16];
    for (int c = 0; c < 4; c++) {
        u32 k = a->rk[c];
        for (int r = 0; r < 4; r++) s[4 * c + r] = in[4 * c + r] ^ (u8)(k >> (24 - 8 * r));
    }
    for (int round = 1; round <= 10; round++) {
        u8 t[16];
        /* SubBytes + ShiftRows (column-major state: s[4*col + row]) */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * c + r] = sbox[s[4 * ((c + r) & 3) + r]];
        /* MixColumns (not in the last round) */
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                u8 *col = t + 4 * c;
                u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int c = 0; c < 4; c++) {
            u32 k = a->rk[4 * round + c];
            for (int r = 0; r < 4; r++) s[4 * c + r] = t[4 * c + r] ^ (u8)(k >> (24 - 8 * r));
        }
    }
    cpy(out, s, 16);
}

/* GF(2^128) multiply, bit by bit (SP 800-38D Algorithm 1) */
static void gf_mul(u64 x[2], const u64 y[2])
{
    u64 zh = 0, zl = 0, vh = y[0], vl = y[1];
    for (int i = 0; i < 128; i++) {
        u64 bit = (i < 64 ? x[0] >> (63 - i) : x[1] >> (127 - i)) & 1;
        u64 mask = 0 - bit;
        zh ^= vh & mask; zl ^= vl & mask;
        u64 lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ ((0 - lsb) & 0xE100000000000000ull);
    }
    x[0] = zh; x[1] = zl;
}

static void ghash_update(u64 acc[2], const u64 h[2], const u8 *data, size_t len)
{
    while (len) {
        u8 block[16];
        zero(block, 16);
        size_t n = len < 16 ? len : 16;
        cpy(block, data, n);
        acc[0] ^= be64(block); acc[1] ^= be64(block + 8);
        gf_mul(acc, h);
        data += n; len -= n;
    }
}

static void gcm_ctr(const struct aes128 *a, const u8 j0[16], const u8 *in, u8 *out, size_t len)
{
    u8 ctr[16], ks[16];
    cpy(ctr, j0, 16);
    while (len) {
        u32 c = be32(ctr + 12) + 1;
        put_be32(ctr + 12, c);
        aes128_encrypt(a, ctr, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
}

static void gcm_tag(const struct aes128 *a, const u8 j0[16], const u8 *ad, size_t adlen,
                    const u8 *ct, size_t len, u8 tag[16])
{
    u8 hb[16], zeroes[16], lens[16], ek[16];
    zero(zeroes, 16);
    aes128_encrypt(a, zeroes, hb);
    u64 h[2] = { be64(hb), be64(hb + 8) }, acc[2] = { 0, 0 };
    ghash_update(acc, h, ad, adlen);
    ghash_update(acc, h, ct, len);
    put_be64(lens, (u64)adlen * 8);
    put_be64(lens + 8, (u64)len * 8);
    ghash_update(acc, h, lens, 16);
    aes128_encrypt(a, j0, ek);
    u8 s[16];
    put_be64(s, acc[0]); put_be64(s + 8, acc[1]);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ek[i];
}

void aes128_gcm_seal(const u8 key[16], const u8 nonce[12], const u8 *ad, size_t adlen,
                     const u8 *pt, size_t len, u8 *out)
{
    struct aes128 a;
    aes128_init(&a, key);
    u8 j0[16];
    cpy(j0, nonce, 12);
    put_be32(j0 + 12, 1);
    gcm_ctr(&a, j0, pt, out, len);
    gcm_tag(&a, j0, ad, adlen, out, len, out + len);
}

int aes128_gcm_open(const u8 key[16], const u8 nonce[12], const u8 *ad, size_t adlen,
                    const u8 *in, size_t len_with_tag, u8 *pt)
{
    if (len_with_tag < 16) return -1;
    size_t len = len_with_tag - 16;
    struct aes128 a;
    aes128_init(&a, key);
    u8 j0[16], tag[16];
    cpy(j0, nonce, 12);
    put_be32(j0 + 12, 1);
    gcm_tag(&a, j0, ad, adlen, in, len, tag);
    if (crypto_memcmp(tag, in + len, 16)) return -1;
    gcm_ctr(&a, j0, in, pt, len);
    return 0;
}

/* ======================================================================
 * SHA-512 (FIPS 180-4)
 * ==================================================================== */
static const u64 K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static u64 rotr64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_block(struct sha512 *s, const u8 *p)
{
    u64 w[80];
    for (int i = 0; i < 16; i++) w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        u64 s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u64 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 80; i++) {
        u64 t1 = h + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        u64 t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha512_init(struct sha512 *s)
{
    static const u64 iv[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                               0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                               0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
    cpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

void sha512_update(struct sha512 *s, const void *data, size_t len)
{
    const u8 *p = data;
    s->len += len;
    while (len) {
        u32 take = 128 - s->n;
        if (take > len) take = (u32)len;
        cpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 128) { sha512_block(s, s->buf); s->n = 0; }
    }
}

void sha512_final(struct sha512 *s, u8 out[64])
{
    u64 bits = s->len * 8;
    u8 pad = 0x80, z = 0;
    sha512_update(s, &pad, 1);
    while (s->n != 112) sha512_update(s, &z, 1);
    u8 l[16];
    zero(l, 8);
    put_be64(l + 8, bits);
    sha512_update(s, l, 16);
    for (int i = 0; i < 8; i++) put_be64(out + 8 * i, s->h[i]);
}

void sha512(const void *data, size_t len, u8 out[64])
{
    struct sha512 s;
    sha512_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}

/* ======================================================================
 * Ed25519 (RFC 8032) on the twisted Edwards curve -x^2 + y^2 = 1 + d x^2 y^2,
 * extended coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, xy = T/Z.
 * Constants are derived from their definitions at first use.
 * ==================================================================== */
struct ge { fe X, Y, Z, T; };

static fe ed_d, ed_2d, ed_sqrtm1;
static struct ge ed_base;
static int ed_ready;

static void fe_copy(fe h, const fe a) { cpy(h, a, sizeof(fe)); }
static void fe_set(fe h, u64 v) { h[0] = v; h[1] = h[2] = h[3] = h[4] = 0; }
static void fe_neg(fe h, const fe a) { fe z; fe_set(z, 0); fe_sub(h, z, a); }
static int fe_isodd(const fe a) { u8 s[32]; fe_tobytes(s, a); return s[0] & 1; }
static int fe_equal(const fe a, const fe b)
{
    u8 x[32], y[32];
    fe_tobytes(x, a); fe_tobytes(y, b);
    return crypto_memcmp(x, y, 32) == 0;
}

/* z^(2^252 - 3): bits 251..2 set, bit 1 clear, bit 0 set */
static void fe_pow2523(fe out, const fe z)
{
    fe r;
    fe_set(r, 1);
    for (int i = 251; i >= 0; i--) {
        fe_sq(r, r);
        if (i >= 2 || i == 0) fe_mul(r, r, z);
    }
    fe_copy(out, r);
}

static void ge_identity(struct ge *p) { fe_set(p->X, 0); fe_set(p->Y, 1); fe_set(p->Z, 1); fe_set(p->T, 0); }

/* unified addition (works for doubling too), RFC 8032 section 5.1.4 */
static void ge_add(struct ge *r, const struct ge *p, const struct ge *q)
{
    fe a, b, c, d, e, f, g, h, t;
    fe_sub(a, p->Y, p->X); fe_sub(t, q->Y, q->X); fe_mul(a, a, t);
    fe_add(b, p->Y, p->X); fe_add(t, q->Y, q->X); fe_mul(b, b, t);
    fe_mul(c, p->T, q->T); fe_mul(c, c, ed_2d);
    fe_mul(d, p->Z, q->Z); fe_add(d, d, d);
    fe_sub(e, b, a); fe_sub(f, d, c); fe_add(g, d, c); fe_add(h, b, a);
    fe_mul(r->X, e, f); fe_mul(r->Y, g, h); fe_mul(r->T, e, h); fe_mul(r->Z, f, g);
}

/* scalar (32 bytes, little endian) times point: double-and-add */
static void ge_scalarmult(struct ge *r, const u8 k[32], const struct ge *p)
{
    struct ge acc;
    ge_identity(&acc);
    for (int i = 255; i >= 0; i--) {
        ge_add(&acc, &acc, &acc);
        if ((k[i >> 3] >> (i & 7)) & 1) ge_add(&acc, &acc, p);
    }
    *r = acc;
}

static void ge_encode(u8 out[32], const struct ge *p)
{
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(out, y);
    out[31] |= (u8)(fe_isodd(x) << 7);
}

/* RFC 8032 section 5.1.3; returns 0 on success */
static int ge_decode(struct ge *p, const u8 in[32])
{
    fe u, v, v3, x, vx2, t;
    int sign = in[31] >> 7;
    u8 ycheck[32];
    fe_frombytes(p->Y, in);                    /* drops the sign bit */
    fe_tobytes(ycheck, p->Y);                  /* reject y >= p (non-canonical) */
    u8 masked[32];
    cpy(masked, in, 32);
    masked[31] &= 0x7F;
    if (crypto_memcmp(ycheck, masked, 32)) return -1;
    fe_set(p->Z, 1);
    fe_sq(u, p->Y);
    fe_mul(v, u, ed_d);
    fe_sub(u, u, p->Z);                        /* u = y^2 - 1 */
    fe_add(v, v, p->Z);                        /* v = d y^2 + 1 */
    fe_sq(v3, v); fe_mul(v3, v3, v);           /* v^3 */
    fe_sq(x, v3); fe_mul(x, x, v); fe_mul(x, x, u);   /* u v^7 */
    fe_pow2523(x, x);
    fe_mul(x, x, v3); fe_mul(x, x, u);         /* x = u v^3 (u v^7)^((p-5)/8) */
    fe_sq(vx2, x); fe_mul(vx2, vx2, v);
    if (!fe_equal(vx2, u)) {
        fe_neg(t, u);
        if (!fe_equal(vx2, t)) return -1;
        fe_mul(x, x, ed_sqrtm1);
    }
    fe_set(t, 0);
    if (fe_equal(x, t) && sign) return -1;
    if (fe_isodd(x) != sign) fe_neg(x, x);
    fe_copy(p->X, x);
    fe_mul(p->T, p->X, p->Y);
    return 0;
}

static void ed_init(void)
{
    if (ed_ready) return;
    fe a, b;
    /* d = -121665 / 121666 */
    fe_set(a, 121665); fe_set(b, 121666);
    fe_invert(b, b);
    fe_mul(a, a, b);
    fe_neg(ed_d, a);
    fe_add(ed_2d, ed_d, ed_d);
    /* sqrt(-1) = 2^((p-1)/4); (p-1)/4 = 2^253 - 5: bits 252..3 set, then 011 */
    fe r, two;
    fe_set(r, 1); fe_set(two, 2);
    for (int i = 252; i >= 0; i--) {
        fe_sq(r, r);
        if (i >= 3 || i == 1 || i == 0) fe_mul(r, r, two);
    }
    fe_copy(ed_sqrtm1, r);
    /* base point: y = 4/5, x even */
    u8 enc[32];
    fe_set(a, 4); fe_set(b, 5);
    fe_invert(b, b);
    fe_mul(a, a, b);
    fe_tobytes(enc, a);
    ed_ready = 1;
    ge_decode(&ed_base, enc);
}

/* ---- scalars modulo L = 2^252 + 27742317777372353535851937790883648493 ---- */
static const u64 ED_L[4] = { 0x5812631a5cf5d3edull, 0x14def9dea2f79cd6ull, 0, 0x1000000000000000ull };

static int sc_geq_l(const u64 r[4])
{
    for (int i = 3; i >= 0; i--) {
        if (r[i] > ED_L[i]) return 1;
        if (r[i] < ED_L[i]) return 0;
    }
    return 1;
}

static void sc_sub_l(u64 r[4])
{
    u64 borrow = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)r[i] - ED_L[i] - borrow;
        r[i] = (u64)t;
        borrow = (u64)(t >> 127) & 1;
    }
}

/* reduce a little-endian number of `len` bytes mod L, bit by bit */
static void sc_reduce(u8 out[32], const u8 *in, int len)
{
    u64 r[4] = { 0, 0, 0, 0 };
    for (int i = len * 8 - 1; i >= 0; i--) {
        r[3] = (r[3] << 1) | (r[2] >> 63);
        r[2] = (r[2] << 1) | (r[1] >> 63);
        r[1] = (r[1] << 1) | (r[0] >> 63);
        r[0] = (r[0] << 1) | ((in[i >> 3] >> (i & 7)) & 1);
        if (sc_geq_l(r)) sc_sub_l(r);
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) out[8 * i + j] = (u8)(r[i] >> (8 * j));
}

/* out = (a * b + c) mod L, all 32-byte little-endian */
static void sc_muladd(u8 out[32], const u8 a[32], const u8 b[32], const u8 c[32])
{
    u64 x[4], y[4], z[9] = { 0 };
    for (int i = 0; i < 4; i++) {
        x[i] = 0; y[i] = 0;
        for (int j = 7; j >= 0; j--) { x[i] = x[i] << 8 | a[8 * i + j]; y[i] = y[i] << 8 | b[8 * i + j]; }
    }
    for (int i = 0; i < 4; i++) {
        u128 carry = 0;
        for (int j = 0; j < 4; j++) {
            u128 t = (u128)x[i] * y[j] + z[i + j] + carry;
            z[i + j] = (u64)t;
            carry = t >> 64;
        }
        z[i + 4] += (u64)carry;
    }
    u128 carry = 0;                            /* + c */
    for (int i = 0; i < 9; i++) {
        u64 ci = 0;
        if (i < 4) for (int j = 7; j >= 0; j--) ci = ci << 8 | c[8 * i + j];
        u128 t = (u128)z[i] + ci + carry;
        z[i] = (u64)t;
        carry = t >> 64;
    }
    u8 wide[72];
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 8; j++) wide[8 * i + j] = (u8)(z[i] >> (8 * j));
    sc_reduce(out, wide, 72);
}

int ed25519_verify(const u8 sig[64], const void *msg, size_t len, const u8 pub[32])
{
    ed_init();
    struct ge A, R, sB, kA, rhs;
    if (ge_decode(&A, pub) || ge_decode(&R, sig)) return -1;
    /* S must be canonical (< L) */
    u64 s[4];
    for (int i = 0; i < 4; i++) { s[i] = 0; for (int j = 7; j >= 0; j--) s[i] = s[i] << 8 | sig[32 + 8 * i + j]; }
    if (sc_geq_l(s)) return -1;
    u8 h[64], k[32];
    struct sha512 st;
    sha512_init(&st);
    sha512_update(&st, sig, 32);
    sha512_update(&st, pub, 32);
    sha512_update(&st, msg, len);
    sha512_final(&st, h);
    sc_reduce(k, h, 64);
    ge_scalarmult(&sB, sig + 32, &ed_base);
    ge_scalarmult(&kA, k, &A);
    ge_add(&rhs, &R, &kA);
    u8 e1[32], e2[32];
    ge_encode(e1, &sB);
    ge_encode(e2, &rhs);
    return crypto_memcmp(e1, e2, 32) ? -1 : 0;
}

void ed25519_public_key(u8 pub[32], const u8 seed[32])
{
    ed_init();
    u8 h[64];
    sha512(seed, 32, h);
    h[0] &= 248; h[31] &= 127; h[31] |= 64;
    struct ge A;
    ge_scalarmult(&A, h, &ed_base);
    ge_encode(pub, &A);
    zero(h, 64);
}

void ed25519_sign(u8 sig[64], const void *msg, size_t len, const u8 seed[32], const u8 pub[32])
{
    ed_init();
    u8 h[64], r[32], k[32], hr[64];
    sha512(seed, 32, h);
    h[0] &= 248; h[31] &= 127; h[31] |= 64;
    struct sha512 st;
    sha512_init(&st);
    sha512_update(&st, h + 32, 32);
    sha512_update(&st, msg, len);
    sha512_final(&st, hr);
    sc_reduce(r, hr, 64);
    struct ge R;
    ge_scalarmult(&R, r, &ed_base);
    ge_encode(sig, &R);
    sha512_init(&st);
    sha512_update(&st, sig, 32);
    sha512_update(&st, pub, 32);
    sha512_update(&st, msg, len);
    sha512_final(&st, hr);
    sc_reduce(k, hr, 64);
    sc_muladd(sig + 32, k, h, r);              /* S = r + k*a mod L */
    zero(h, 64);
    zero(r, 32);
}
