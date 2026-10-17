/*
 * NimbusOS - picture decoders for the Web Browser: PNG, GIF and baseline JPEG.
 *
 * Everything is integer arithmetic (the kernel is built without floating
 * point).  PNG: zlib inflate, the five scanline filters, all colour types and
 * bit depths except interlaced files.  GIF: LZW, the first frame, with
 * transparency.  JPEG: sequential Huffman (baseline), 8-bit, grey or YCbCr
 * with any common chroma subsampling and restart markers; progressive files are
 * not supported.  Transparent pixels are blended with white, the page colour.
 */
#include "gui/imgdec.h"
#include "gui/bmp.h"
#include "kernel/heap.h"
#include "kernel/string.h"

#define MAX_PIXELS (4096 * 4096)

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* a pixel over white */
static u32 over_white(int r, int g, int b, int a)
{
    if (a < 255) {
        r = (r * a + 255 * (255 - a)) / 255;
        g = (g * a + 255 * (255 - a)) / 255;
        b = (b * a + 255 * (255 - a)) / 255;
    }
    return RGB(r, g, b);
}

/* ------------------------------------------------------------------ inflate */
struct inf {
    const u8 *in;
    u32 inlen, inpos;
    u32 bitbuf;
    int bitcnt;
    u8 *out;
    u32 outcap, outpos;
    bool bad;
};

static int inf_bits(struct inf *s, int need)
{
    u32 v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->bad = true; return 0; }
        v |= (u32)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

struct huff { u16 count[16]; u16 symbol[288]; };

static void huff_build(struct huff *h, const u8 *len, int n)
{
    u16 offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (u16)i;
}

/* canonical Huffman decoding, one bit at a time (after zlib's "puff") */
static int huff_decode(struct inf *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= inf_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->bad) return -1;
    }
    return -1;
}

static bool inf_codes(struct inf *s, const struct huff *lit, const struct huff *dist)
{
    static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const u16 lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const u16 dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0 || s->bad) return false;
        if (sym < 256) {
            if (s->outpos >= s->outcap) return false;
            s->out[s->outpos++] = (u8)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            u32 len = lbase[sym] + inf_bits(s, lext[sym]);
            int ds = huff_decode(s, dist);
            if (ds < 0 || ds >= 30) return false;
            u32 d = dbase[ds] + inf_bits(s, dext[ds]);
            if (s->bad || d > s->outpos || s->outpos + len > s->outcap) return false;
            for (u32 i = 0; i < len; i++) { s->out[s->outpos] = s->out[s->outpos - d]; s->outpos++; }
        }
    }
}

/* zlib stream -> out (at most outcap bytes); returns the bytes written or -1 */
static int inflate_zlib(const u8 *in, u32 inlen, u8 *out, u32 outcap)
{
    if (inlen < 2) return -1;
    struct inf s = { in + 2, inlen - 2, 0, 0, 0, out, outcap, 0, false };
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.bad) return -1;
        if (type == 0) {                                /* stored */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) return -1;
            u32 len = s.in[s.inpos] | s.in[s.inpos + 1] << 8;
            s.inpos += 4;
            if (s.inpos + len > s.inlen || s.outpos + len > s.outcap) return -1;
            memcpy(s.out + s.outpos, s.in + s.inpos, len);
            s.inpos += len;
            s.outpos += len;
        } else if (type == 1 || type == 2) {
            static struct huff lit, dist;               /* the decoder is only used from the GUI task */
            u8 lens[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) lens[i] = 8;
                for (; i < 256; i++) lens[i] = 9;
                for (; i < 280; i++) lens[i] = 7;
                for (; i < 288; i++) lens[i] = 8;
                huff_build(&lit, lens, 288);
                for (i = 0; i < 30; i++) lens[i] = 5;
                huff_build(&dist, lens, 30);
            } else {
                static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                int nlen = inf_bits(&s, 5) + 257, ndist = inf_bits(&s, 5) + 1, ncode = inf_bits(&s, 4) + 4;
                if (s.bad || nlen > 286 || ndist > 30) return -1;
                u8 cl[19];
                memset(cl, 0, sizeof cl);
                for (int i = 0; i < ncode; i++) cl[order[i]] = (u8)inf_bits(&s, 3);
                struct huff lencode;
                huff_build(&lencode, cl, 19);
                int idx = 0;
                while (idx < nlen + ndist) {
                    int sym = huff_decode(&s, &lencode);
                    if (sym < 0 || s.bad) return -1;
                    if (sym < 16) lens[idx++] = (u8)sym;
                    else {
                        int rep, val = 0;
                        if (sym == 16) { if (!idx) return -1; val = lens[idx - 1]; rep = 3 + inf_bits(&s, 2); }
                        else if (sym == 17) rep = 3 + inf_bits(&s, 3);
                        else rep = 11 + inf_bits(&s, 7);
                        if (idx + rep > nlen + ndist) return -1;
                        while (rep--) lens[idx++] = (u8)val;
                    }
                }
                huff_build(&lit, lens, nlen);
                huff_build(&dist, lens + nlen, ndist);
            }
            if (!inf_codes(&s, &lit, &dist)) return -1;
        } else return -1;
    } while (!last);
    return (int)s.outpos;
}

/* ---------------------------------------------------------------------- PNG */
static int png_sample(const u8 *row, int depth, u32 index)
{
    if (depth == 8) return row[index];
    if (depth == 16) return row[index * 2];                    /* the high byte */
    int per = 8 / depth;
    int shift = 8 - depth * (int)(index % per + 1);
    return (row[index / per] >> shift) & ((1 << depth) - 1);
}

static surface_t *png_decode(const u8 *d, u32 n)
{
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    u32 w = 0, h = 0, pos = 8, idatlen = 0, idatcap = 0;
    int depth = 0, ctype = -1, interlace = 0, npal = 0;
    u8 pal[256][3], trns[256];
    u8 *idat = NULL;
    memset(trns, 255, sizeof trns);
    while (pos + 12 <= n) {
        u32 clen = be32(d + pos);
        const u8 *type = d + pos + 4, *cd = d + pos + 8;
        if (clen > n - pos - 12) break;
        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            w = be32(cd); h = be32(cd + 4); depth = cd[8]; ctype = cd[9]; interlace = cd[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(clen / 3 > 256 ? 256 : clen / 3);
            for (int i = 0; i < npal; i++) { pal[i][0] = cd[i * 3]; pal[i][1] = cd[i * 3 + 1]; pal[i][2] = cd[i * 3 + 2]; }
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
            for (u32 i = 0; i < clen && i < 256; i++) trns[i] = cd[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idatlen + clen > idatcap) {
                idatcap = (idatlen + clen) * 2;
                u8 *ni = krealloc(idat, idatcap);
                if (!ni) { kfree(idat); return NULL; }
                idat = ni;
            }
            memcpy(idat + idatlen, cd, clen);
            idatlen += clen;
        } else if (!memcmp(type, "IEND", 4)) break;
        pos += 12 + clen;
    }
    surface_t *s = NULL;
    u8 *raw = NULL;
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    bool ok = idat && channels && w && h && (u64)w * h <= MAX_PIXELS && !interlace &&
              (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) && !(ctype == 3 && !npal) &&
              (depth >= 8 || ctype == 0 || ctype == 3) && (depth <= 8 || ctype != 3);
    if (!ok) goto done;
    u32 bits = (u32)channels * depth, stride = (w * bits + 7) / 8, rawsize = h * (stride + 1);
    raw = kmalloc(rawsize);
    if (!raw || inflate_zlib(idat, idatlen, raw, rawsize) != (int)rawsize) goto done;
    int bpp = bits >= 8 ? (int)(bits / 8) : 1;
    for (u32 y = 0; y < h; y++) {                                       /* undo the scanline filters */
        u8 *row = raw + y * (stride + 1) + 1;
        const u8 *up = y ? row - (stride + 1) : NULL;
        int f = row[-1];
        for (u32 i = 0; i < stride; i++) {
            int a = i >= (u32)bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= (u32)bpp) ? up[i - bpp] : 0;
            int v = row[i];
            switch (f) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            }
            row[i] = (u8)v;
        }
    }
    s = surface_create((int)w, (int)h);
    if (!s) goto done;
    for (u32 y = 0; y < h; y++) {
        const u8 *row = raw + y * (stride + 1) + 1;
        u32 *dst = s->px + y * s->pitch;
        for (u32 x = 0; x < w; x++) {
            int r, g, b, a = 255;
            if (ctype == 3) {
                int idx = png_sample(row, depth, x);
                if (idx >= npal) idx = 0;
                r = pal[idx][0]; g = pal[idx][1]; b = pal[idx][2]; a = trns[idx];
            } else if (ctype == 0 || ctype == 4) {
                int v = png_sample(row, depth, x * channels);
                if (depth < 8) v = v * 255 / ((1 << depth) - 1);
                r = g = b = v;
                if (ctype == 4) a = png_sample(row, depth, x * channels + 1);
            } else {
                r = png_sample(row, depth, x * channels);
                g = png_sample(row, depth, x * channels + 1);
                b = png_sample(row, depth, x * channels + 2);
                if (ctype == 6) a = png_sample(row, depth, x * channels + 3);
            }
            dst[x] = over_white(r, g, b, a);
        }
    }
done:
    kfree(raw);
    kfree(idat);
    return s;
}

/* ---------------------------------------------------------------------- GIF */
static bool lzw_decode(const u8 *data, u32 dlen, int mincode, u8 *out, u32 outlen)
{
    if (mincode < 2 || mincode > 8) return false;
    u16 *prefix = kmalloc(4096 * sizeof(u16));
    u8 *suffix = kmalloc(4096), *stack = kmalloc(4097);
    if (!prefix || !suffix || !stack) { kfree(prefix); kfree(suffix); kfree(stack); return false; }
    int clear = 1 << mincode, eoi = clear + 1, next = clear + 2, size = mincode + 1, prev = -1;
    u32 bitbuf = 0, pos = 0, o = 0;
    int bitcnt = 0;
    u8 first = 0;
    for (;;) {
        while (bitcnt < size) {
            if (pos >= dlen) goto end;
            bitbuf |= (u32)data[pos++] << bitcnt;
            bitcnt += 8;
        }
        int code = (int)(bitbuf & ((1u << size) - 1));
        bitbuf >>= size;
        bitcnt -= size;
        if (code == clear) { next = clear + 2; size = mincode + 1; prev = -1; continue; }
        if (code == eoi) break;
        if (prev < 0) {                                 /* the first code after a clear is a plain colour index */
            if (code >= clear) break;
            if (o < outlen) out[o++] = (u8)code;
            first = (u8)code;
            prev = code;
            continue;
        }
        int sp = 0, cur = code;
        if (code >= next) {                             /* the string being defined right now */
            if (code > next) break;
            stack[sp++] = first;
            cur = prev;
        }
        while (cur >= clear && sp < 4096) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
        first = (u8)cur;
        stack[sp++] = (u8)cur;
        while (sp) { if (o < outlen) out[o++] = stack[--sp]; else sp--; }
        if (next < 4096) {
            prefix[next] = (u16)prev;
            suffix[next] = first;
            next++;
            if (next == (1 << size) && size < 12) size++;
        }
        prev = code;
    }
end:
    kfree(prefix); kfree(suffix); kfree(stack);
    return true;
}

static surface_t *gif_decode(const u8 *d, u32 n)
{
    if (n < 13 || (memcmp(d, "GIF87a", 6) && memcmp(d, "GIF89a", 6))) return NULL;
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8, flags = d[10], transp = -1;
    u8 gpal[256][3], lpal[256][3];
    int gn = 0;
    u32 pos = 13;
    if (flags & 0x80) {
        gn = 2 << (flags & 7);
        if (pos + 3 * (u32)gn > n) return NULL;
        memcpy(gpal, d + pos, 3 * gn);
        pos += 3 * gn;
    }
    while (pos < n) {
        u8 b = d[pos++];
        if (b == 0x21) {                                /* extension */
            if (pos >= n) break;
            u8 label = d[pos++];
            if (label == 0xF9 && pos + 5 <= n && (d[pos + 1] & 1)) transp = d[pos + 4];
            while (pos < n) { u32 sz = d[pos++]; if (!sz) break; pos += sz; }
        } else if (b == 0x2C) {                         /* the first image */
            if (pos + 9 > n) return NULL;
            int left = d[pos] | d[pos + 1] << 8, top = d[pos + 2] | d[pos + 3] << 8;
            int iw = d[pos + 4] | d[pos + 5] << 8, ih = d[pos + 6] | d[pos + 7] << 8, lf = d[pos + 8];
            pos += 9;
            const u8 (*pal)[3] = gpal;
            int pn = gn;
            if (lf & 0x80) {
                pn = 2 << (lf & 7);
                if (pos + 3 * (u32)pn > n) return NULL;
                memcpy(lpal, d + pos, 3 * pn);
                pos += 3 * pn;
                pal = lpal;
            }
            if (!pn || iw <= 0 || ih <= 0 || (u64)iw * ih > MAX_PIXELS || pos >= n) return NULL;
            int mincode = d[pos++];
            u8 *data = kmalloc(n - pos + 1);
            if (!data) return NULL;
            u32 dlen = 0;
            while (pos < n) {                           /* join the sub-blocks */
                u32 sz = d[pos++];
                if (!sz) break;
                if (pos + sz > n) sz = n - pos;
                memcpy(data + dlen, d + pos, sz);
                dlen += sz;
                pos += sz;
            }
            u8 *idx = kzalloc((size_t)iw * ih);
            bool ok = idx && lzw_decode(data, dlen, mincode, idx, (u32)iw * ih);
            kfree(data);
            if (!ok) { kfree(idx); return NULL; }
            if (sw <= 0 || sh <= 0) { sw = left + iw; sh = top + ih; }
            if ((u64)sw * sh > MAX_PIXELS) { kfree(idx); return NULL; }
            surface_t *s = surface_create(sw, sh);
            if (!s) { kfree(idx); return NULL; }
            gfx_fill_rect(s, 0, 0, sw, sh, 0xFFFFFF);
            static const int pstart[4] = { 0, 4, 2, 1 }, pstep[4] = { 8, 8, 4, 2 };
            int pass = 0, y = 0;
            for (int r = 0; r < ih; r++) {
                int dy = top + y;
                for (int x = 0; x < iw; x++) {
                    int c = idx[r * iw + x], dx = left + x;
                    if (c == transp || c >= pn || dx < 0 || dx >= sw || dy < 0 || dy >= sh) continue;
                    s->px[dy * s->pitch + dx] = RGB(pal[c][0], pal[c][1], pal[c][2]);
                }
                if (lf & 0x40) {                        /* interlaced: four passes of every 8th, 8th, 4th and 2nd row */
                    y += pstep[pass];
                    while (y >= ih && pass < 3) { pass++; y = pstart[pass]; }
                } else y++;
            }
            kfree(idx);
            return s;
        } else break;
    }
    return NULL;
}

/* --------------------------------------------------------------------- JPEG */
static const u8 zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* idct_t[x][u] = 4096 * C(u)/2 * cos((2x+1) u pi / 16) */
static const int idct_t[8][8] = {
    {  1448,  2009,  1892,  1703,  1448,  1138,   784,   400 },
    {  1448,  1703,   784,  -400, -1448, -2009, -1892, -1138 },
    {  1448,  1138,  -784, -2009, -1448,   400,  1892,  1703 },
    {  1448,   400, -1892, -1138,  1448,  1703,  -784, -2009 },
    {  1448,  -400, -1892,  1138,  1448, -1703,  -784,  2009 },
    {  1448, -1138,  -784,  2009, -1448,  -400,  1892, -1703 },
    {  1448, -1703,   784,   400, -1448,  2009, -1892,  1138 },
    {  1448, -2009,  1892, -1703,  1448, -1138,   784,  -400 },
};

struct jhuff { u8 bits[17], vals[256]; int mincode[17], maxcode[17], valptr[17]; bool ok; };
struct jcomp { int id, h, v, tq, td, ta, pred, pw, ph; u8 *px; };

struct jpeg {
    const u8 *d;
    u32 n, pos;
    u32 bitbuf;
    int bitcnt;
};

static int jbit(struct jpeg *j)
{
    if (j->bitcnt == 0) {
        u32 b = 0;
        if (j->pos < j->n) {
            b = j->d[j->pos];
            if (b == 0xFF) {
                if (j->pos + 1 < j->n && j->d[j->pos + 1] == 0) j->pos += 2;       /* a stuffed 0xFF data byte */
                else b = 0;                                                       /* a marker: feed zeros, do not consume it */
            } else j->pos++;
        }
        j->bitbuf = b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jbits(struct jpeg *j, int n)
{
    int v = 0;
    while (n--) v = v << 1 | jbit(j);
    return v;
}

static void jhuff_build(struct jhuff *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        k += h->bits[l];
        code <<= 1;
    }
    h->ok = true;
}

static int jdecode(struct jpeg *j, const struct jhuff *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = code << 1 | jbit(j);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return -1;
}

static int jext(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

/* one 8x8 block: Huffman decode, dequantise, inverse DCT into the component plane */
static bool jblock(struct jpeg *j, struct jcomp *c, const struct jhuff *dc, const struct jhuff *ac, const u16 *q, int px, int py)
{
    int blk[64];
    memset(blk, 0, sizeof blk);
    int t = jdecode(j, dc);
    if (t < 0 || t > 11) return false;
    c->pred += jext(jbits(j, t), t);
    blk[0] = c->pred * q[0];
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, ac);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k > 63) return false;
        blk[zz[k]] = jext(jbits(j, s), s) * q[k];
        k++;
    }
    int tmp[64];
    for (int v = 0; v < 8; v++)                         /* rows */
        for (int x = 0; x < 8; x++) {
            int sum = 0;
            for (int u = 0; u < 8; u++) sum += idct_t[x][u] * blk[v * 8 + u];
            tmp[v * 8 + x] = (sum + 2048) >> 12;
        }
    for (int x = 0; x < 8; x++)                         /* columns */
        for (int y = 0; y < 8; y++) {
            int sum = 0;
            for (int v = 0; v < 8; v++) sum += idct_t[y][v] * tmp[v * 8 + x];
            int val = ((sum + 2048) >> 12) + 128;
            c->px[(py + y) * c->pw + px + x] = (u8)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
    return true;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static surface_t *jpeg_decode(const u8 *d, u32 n)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;
    static struct jhuff dct[4], act[4];
    u16 q[4][64];
    struct jcomp comp[3];
    int ncomp = 0, w = 0, h = 0, hmax = 1, vmax = 1, ri = 0;
    bool frame = false;
    surface_t *s = NULL;
    memset(dct, 0, sizeof dct);
    memset(act, 0, sizeof act);
    memset(q, 0, sizeof q);
    memset(comp, 0, sizeof comp);
    u32 pos = 2;
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) { pos++; continue; }
        u8 m = d[pos + 1];
        if (m == 0xFF) { pos++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0x00) { pos += 2; continue; }
        if (m == 0xD9) break;
        u32 len = d[pos + 2] << 8 | d[pos + 3], seg = pos + 4, end = pos + 2 + len;
        if (len < 2 || end > n) goto fail;
        if (m == 0xDB) {                                /* quantisation tables */
            while (seg < end) {
                int pq = d[seg] >> 4, tq = d[seg] & 15;
                seg++;
                if (tq > 3 || seg + (pq ? 128u : 64u) > end) goto fail;
                for (int i = 0; i < 64; i++) { q[tq][i] = pq ? (u16)(d[seg] << 8 | d[seg + 1]) : d[seg]; seg += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {                         /* Huffman tables */
            while (seg + 17 <= end) {
                int tc = d[seg] >> 4, th = d[seg] & 15;
                if (th > 3 || tc > 1) goto fail;
                struct jhuff *t = tc ? &act[th] : &dct[th];
                int total = 0;
                t->bits[0] = 0;
                for (int i = 1; i <= 16; i++) { t->bits[i] = d[seg + i]; total += t->bits[i]; }
                seg += 17;
                if (total > 256 || seg + total > end) goto fail;
                memcpy(t->vals, d + seg, total);
                seg += total;
                jhuff_build(t);
            }
        } else if (m == 0xC0 || m == 0xC1) {            /* baseline / extended sequential, Huffman */
            if (len < 8 || d[seg] != 8) goto fail;
            h = d[seg + 1] << 8 | d[seg + 2];
            w = d[seg + 3] << 8 | d[seg + 4];
            ncomp = d[seg + 5];
            if ((ncomp != 1 && ncomp != 3) || w <= 0 || h <= 0 || (u64)w * h > MAX_PIXELS || len < 8 + 3u * ncomp) goto fail;
            for (int i = 0; i < ncomp; i++) {
                comp[i].id = d[seg + 6 + i * 3];
                comp[i].h = d[seg + 7 + i * 3] >> 4;
                comp[i].v = d[seg + 7 + i * 3] & 15;
                comp[i].tq = d[seg + 8 + i * 3] & 3;
                if (comp[i].h < 1 || comp[i].h > 2 || comp[i].v < 1 || comp[i].v > 2) goto fail;
                hmax = MAX(hmax, comp[i].h);
                vmax = MAX(vmax, comp[i].v);
            }
            frame = true;
        } else if (m == 0xC2 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            goto fail;                                  /* progressive, lossless, arithmetic: not supported */
        } else if (m == 0xDD) {
            ri = d[seg] << 8 | d[seg + 1];
        } else if (m == 0xDA) {                         /* the scan */
            if (!frame) goto fail;
            int ns = d[seg];
            if (ns != ncomp || len < 6 + 2u * ns) goto fail;
            for (int i = 0; i < ns; i++) {
                int cid = d[seg + 1 + i * 2];
                int k = 0;
                for (; k < ncomp; k++) if (comp[k].id == cid) break;
                if (k == ncomp) goto fail;
                comp[k].td = d[seg + 2 + i * 2] >> 4;
                comp[k].ta = d[seg + 2 + i * 2] & 15;
                if (comp[k].td > 3 || comp[k].ta > 3 || !dct[comp[k].td].ok || !act[comp[k].ta].ok) goto fail;
            }
            int mw = 8 * hmax, mh = 8 * vmax, mx = (w + mw - 1) / mw, my = (h + mh - 1) / mh;
            for (int i = 0; i < ncomp; i++) {
                comp[i].pw = mx * 8 * comp[i].h;
                comp[i].ph = my * 8 * comp[i].v;
                comp[i].px = kzalloc((size_t)comp[i].pw * comp[i].ph);
                comp[i].pred = 0;
                if (!comp[i].px) goto fail;
            }
            struct jpeg j = { d, n, end, 0, 0 };
            int count = 0;
            for (int yy = 0; yy < my; yy++)
                for (int xx = 0; xx < mx; xx++) {
                    if (ri && count == ri) {            /* restart marker: byte-align, find RSTn, reset predictors */
                        j.bitcnt = 0;
                        while (j.pos + 1 < n && !(d[j.pos] == 0xFF && d[j.pos + 1] >= 0xD0 && d[j.pos + 1] <= 0xD7)) j.pos++;
                        j.pos += 2;
                        for (int i = 0; i < ncomp; i++) comp[i].pred = 0;
                        count = 0;
                    }
                    for (int i = 0; i < ncomp; i++)
                        for (int by = 0; by < comp[i].v; by++)
                            for (int bx = 0; bx < comp[i].h; bx++)
                                if (!jblock(&j, &comp[i], &dct[comp[i].td], &act[comp[i].ta], q[comp[i].tq],
                                            (xx * comp[i].h + bx) * 8, (yy * comp[i].v + by) * 8)) goto fail;
                    count++;
                }
            s = surface_create(w, h);
            if (!s) goto fail;
            for (int y = 0; y < h; y++) {
                u32 *dst = s->px + y * s->pitch;
                for (int x = 0; x < w; x++) {
                    int v[3];
                    for (int i = 0; i < ncomp; i++) v[i] = comp[i].px[(y * comp[i].v / vmax) * comp[i].pw + x * comp[i].h / hmax];
                    if (ncomp == 1) dst[x] = RGB(v[0], v[0], v[0]);
                    else {
                        int yv = v[0], cb = v[1] - 128, cr = v[2] - 128;
                        dst[x] = RGB(clamp8(yv + ((91881 * cr + 32768) >> 16)),
                                     clamp8(yv - ((22554 * cb + 46802 * cr - 32768) >> 16)),
                                     clamp8(yv + ((116130 * cb + 32768) >> 16)));
                    }
                }
            }
            for (int i = 0; i < ncomp; i++) kfree(comp[i].px);
            return s;
        }
        pos = end;
    }
fail:
    for (int i = 0; i < 3; i++) kfree(comp[i].px);
    if (s) surface_destroy(s);
    return NULL;
}

surface_t *img_decode(const u8 *data, u32 len)
{
    if (len > 8 && !memcmp(data, "\x89PNG", 4)) return png_decode(data, len);
    if (len > 6 && !memcmp(data, "GIF8", 4)) return gif_decode(data, len);
    if (len > 4 && data[0] == 0xFF && data[1] == 0xD8) return jpeg_decode(data, len);
    if (len > 2 && data[0] == 'B' && data[1] == 'M') return bmp_decode(data, len);
    return NULL;
}
