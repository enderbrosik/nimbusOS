/*
 * NimbusOS - TLS 1.3 client (RFC 8446).  See net/tls.h.
 *
 * Flow: ClientHello -> ServerHello (X25519 key share) -> handshake keys ->
 * EncryptedExtensions, Certificate, CertificateVerify (both read but not
 * checked), Finished (checked) -> client Finished -> application keys.
 * Post-handshake NewSessionTicket messages are ignored; KeyUpdate is
 * honoured.
 */
#include "net/tls.h"
#ifdef NIMBUS_KERNEL
#include "kernel/heap.h"
#include "kernel/string.h"
#define tls_alloc(n)  kzalloc(n)
#define tls_freemem   kfree
#else
#include <stdlib.h>
#include <string.h>
#define tls_alloc(n)  calloc(1, n)
#define tls_freemem   free
#endif

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define REC_MAX      (16384 + 256)      /* largest TLSCiphertext fragment */
#define HS_MAX       (64 * 1024)        /* largest handshake message we accept */

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
       HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20,
       HS_KEY_UPDATE = 24 };
#define SUITE_AES128_GCM  0x1301
#define SUITE_CHACHA20    0x1303

struct dir {                            /* one direction's traffic keys */
    bool on;
    u8 secret[32], key[32], iv[12];
    u64 seq;
};

struct tls {
    struct tls_io io;
    char sni[256];
    u16 suite;
    int alert;
    bool closed;
    struct sha256 transcript;
    u8 priv[32];
    u8 hs_secret[32], master[32];
    struct dir rx, tx;
    /* receive side */
    u8 *in;                  /* raw bytes from the transport */
    u32 in_len;
    u8 *rec;                 /* decrypted record */
    u32 rec_len, rec_pos;
    int rec_type;
    u8 *hs;                  /* handshake message reassembly */
    u32 hs_len;
    u32 timeout;
    bool key_update_requested;
};

static void cpy(void *d, const void *s, u32 n) { memcpy(d, s, n); }

const char *tls_strerror(int e)
{
    switch (e) {
    case TLS_EIO: return "connection failed or timed out";
    case TLS_EPROTO: return "TLS protocol error";
    case TLS_EALERT: return "server rejected the TLS connection";
    case TLS_EUNSUPP: return "server requires a TLS feature NimbusOS lacks";
    case TLS_ECRYPT: return "TLS record failed authentication";
    case TLS_ENOMEM: return "out of memory";
    default: return "TLS error";
    }
}

const char *tls_cipher_name(struct tls *t)
{
    return t->suite == SUITE_CHACHA20 ? "TLS 1.3, CHACHA20-POLY1305, X25519" :
           t->suite == SUITE_AES128_GCM ? "TLS 1.3, AES-128-GCM, X25519" : "none";
}

int tls_last_alert(struct tls *t) { return t->alert; }

struct tls *tls_new(const struct tls_io *io, const char *server_name)
{
    struct tls *t = tls_alloc(sizeof *t);
    if (!t) return NULL;
    t->in = tls_alloc(REC_MAX + 5);
    t->rec = tls_alloc(REC_MAX);
    t->hs = tls_alloc(HS_MAX + 4);
    if (!t->in || !t->rec || !t->hs) { tls_free(t); return NULL; }
    t->io = *io;
    t->alert = -1;
    size_t n = server_name ? strlen(server_name) : 0;
    if (n >= sizeof t->sni) n = sizeof t->sni - 1;
    cpy(t->sni, server_name ? server_name : "", (u32)n);
    t->sni[n] = 0;
    sha256_init(&t->transcript);
    return t;
}

void tls_free(struct tls *t)
{
    if (!t) return;
    tls_freemem(t->in);
    tls_freemem(t->rec);
    tls_freemem(t->hs);
    memset(t, 0, sizeof *t);
    tls_freemem(t);
}

/* ---- key schedule helpers --------------------------------------------- */

static void transcript_hash(struct tls *t, u8 out[32])
{
    struct sha256 copy = t->transcript;
    sha256_final(&copy, out);
}

static u32 key_len(struct tls *t) { return t->suite == SUITE_CHACHA20 ? 32 : 16; }

static void set_keys(struct tls *t, struct dir *d, const u8 secret[32])
{
    cpy(d->secret, secret, 32);
    hkdf_expand_label(secret, "key", NULL, 0, d->key, key_len(t));
    hkdf_expand_label(secret, "iv", NULL, 0, d->iv, 12);
    d->seq = 0;
    d->on = true;
}

static void make_nonce(const struct dir *d, u8 nonce[12])
{
    cpy(nonce, d->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (u8)(d->seq >> (8 * i));
}

/* ---- record layer: sending --------------------------------------------- */

static int send_all(struct tls *t, const u8 *p, u32 n)
{
    while (n) {
        int r = t->io.send(t->io.ctx, p, n);
        if (r <= 0) return TLS_EIO;
        p += r; n -= (u32)r;
    }
    return TLS_OK;
}

static int send_record(struct tls *t, int type, const u8 *data, u32 len)
{
    u8 *out = tls_alloc(len + 5 + 1 + 16);
    if (!out) return TLS_ENOMEM;
    u32 n;
    if (!t->tx.on) {
        out[0] = (u8)type; out[1] = 3; out[2] = type == CT_HANDSHAKE ? 1 : 3;
        out[3] = (u8)(len >> 8); out[4] = (u8)len;
        cpy(out + 5, data, len);
        n = len + 5;
    } else {
        /* TLSInnerPlaintext = content || content type; header says application_data */
        u32 inner = len + 1, ct = inner + 16;
        u8 *pt = tls_alloc(inner);
        if (!pt) { tls_freemem(out); return TLS_ENOMEM; }
        cpy(pt, data, len);
        pt[len] = (u8)type;
        out[0] = CT_APPDATA; out[1] = 3; out[2] = 3;
        out[3] = (u8)(ct >> 8); out[4] = (u8)ct;
        u8 nonce[12];
        make_nonce(&t->tx, nonce);
        if (t->suite == SUITE_CHACHA20) chacha20_poly1305_seal(t->tx.key, nonce, out, 5, pt, inner, out + 5);
        else aes128_gcm_seal(t->tx.key, nonce, out, 5, pt, inner, out + 5);
        t->tx.seq++;
        tls_freemem(pt);
        n = ct + 5;
    }
    int r = send_all(t, out, n);
    tls_freemem(out);
    return r;
}

/* ---- record layer: receiving -------------------------------------------- */

/* Read one record into t->rec (decrypted if keys are set). */
static int read_record(struct tls *t)
{
    for (;;) {
        while (t->in_len < 5 || t->in_len < 5 + ((u32)t->in[3] << 8 | t->in[4])) {
            if (t->in_len >= 5 && ((u32)t->in[3] << 8 | t->in[4]) > REC_MAX) return TLS_EPROTO;
            int r = t->io.recv(t->io.ctx, t->in + t->in_len, REC_MAX + 5 - t->in_len, t->timeout);
            if (r <= 0) return t->in_len == 0 && r == 0 ? 0 : TLS_EIO;
            t->in_len += (u32)r;
        }
        int type = t->in[0];
        u32 len = (u32)t->in[3] << 8 | t->in[4];
        u8 hdr[5];
        cpy(hdr, t->in, 5);
        const u8 *body = t->in + 5;
        if (type == CT_CCS) {                 /* compatibility ChangeCipherSpec: ignore */
            memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
            t->in_len -= 5 + len;
            continue;
        }
        if (t->rx.on && type == CT_APPDATA) {
            if (len < 17) return TLS_EPROTO;
            u8 nonce[12];
            make_nonce(&t->rx, nonce);
            int bad = t->suite == SUITE_CHACHA20
                ? chacha20_poly1305_open(t->rx.key, nonce, hdr, 5, body, len, t->rec)
                : aes128_gcm_open(t->rx.key, nonce, hdr, 5, body, len, t->rec);
            if (bad) return TLS_ECRYPT;
            t->rx.seq++;
            u32 n = len - 16;
            while (n && t->rec[n - 1] == 0) n--;      /* strip padding */
            if (!n) return TLS_EPROTO;
            t->rec_type = t->rec[n - 1];
            t->rec_len = n - 1;
        } else {
            if (t->rx.on && type != CT_ALERT) return TLS_EPROTO;
            cpy(t->rec, body, len);
            t->rec_type = type;
            t->rec_len = len;
        }
        memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
        t->in_len -= 5 + len;
        t->rec_pos = 0;
        if (t->rec_type == CT_ALERT) {
            if (t->rec_len >= 2) t->alert = t->rec[1];
            t->closed = true;
            return t->alert == 0 ? 0 : TLS_EALERT;     /* close_notify = clean EOF */
        }
        return 1;
    }
}

/* Next complete handshake message; returns its type, body in t->hs + 4. */
static int next_handshake(struct tls *t, u32 *body_len)
{
    for (;;) {
        if (t->hs_len >= 4) {
            u32 n = (u32)t->hs[1] << 16 | (u32)t->hs[2] << 8 | t->hs[3];
            if (n > HS_MAX) return TLS_EPROTO;
            if (t->hs_len >= 4 + n) { *body_len = n; return t->hs[0]; }
        }
        if (t->rec_pos >= t->rec_len) {
            int r = read_record(t);
            if (r <= 0) return r == 0 ? TLS_EPROTO : r;
            if (t->rec_type != CT_HANDSHAKE) return TLS_EPROTO;
        }
        u32 take = t->rec_len - t->rec_pos;
        if (t->hs_len + take > HS_MAX + 4) return TLS_EPROTO;
        cpy(t->hs + t->hs_len, t->rec + t->rec_pos, take);
        t->hs_len += take;
        t->rec_pos += take;
    }
}

/* Drop the current handshake message, optionally adding it to the transcript. */
static void consume_handshake(struct tls *t, u32 body_len, bool hash)
{
    u32 n = 4 + body_len;
    if (hash) sha256_update(&t->transcript, t->hs, n);
    memmove(t->hs, t->hs + n, t->hs_len - n);
    t->hs_len -= n;
}

/* ---- ClientHello ------------------------------------------------------- */

static bool is_ip_literal(const char *s)
{
    if (!*s) return true;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == ':')) return false;
    return true;
}

static u8 *put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; return p + 2; }

static int send_client_hello(struct tls *t)
{
    u8 msg[1024], *p = msg + 4;          /* handshake header filled in at the end */
    u8 pub[32];
    tls_random_bytes(t->priv, 32);
    x25519_base(pub, t->priv);
    p = put16(p, 0x0303);                /* legacy_version */
    tls_random_bytes(p, 32); p += 32;    /* random */
    *p++ = 32;                           /* legacy_session_id (middlebox compatibility) */
    tls_random_bytes(p, 32); p += 32;
    p = put16(p, 4);
    p = put16(p, SUITE_AES128_GCM);
    p = put16(p, SUITE_CHACHA20);
    *p++ = 1; *p++ = 0;                  /* compression: null */
    u8 *ext_len = p; p += 2;
    u8 *ext_start = p;
    u32 sl = (u32)strlen(t->sni);
    if (sl && !is_ip_literal(t->sni) && sl < 250) {   /* server_name */
        p = put16(p, 0); p = put16(p, sl + 5); p = put16(p, sl + 3);
        *p++ = 0; p = put16(p, sl);
        cpy(p, t->sni, sl); p += sl;
    }
    p = put16(p, 10); p = put16(p, 4); p = put16(p, 2); p = put16(p, 0x001d);   /* supported_groups: x25519 */
    static const u16 sigs[] = { 0x0403, 0x0503, 0x0603, 0x0804, 0x0805, 0x0806, 0x0807,
                                0x0401, 0x0501, 0x0601 };
    p = put16(p, 13); p = put16(p, 2 + 2 * ARRAY_LEN(sigs)); p = put16(p, 2 * ARRAY_LEN(sigs));
    for (u32 i = 0; i < ARRAY_LEN(sigs); i++) p = put16(p, sigs[i]);
    p = put16(p, 43); p = put16(p, 3); *p++ = 2; p = put16(p, 0x0304);      /* supported_versions: 1.3 */
    p = put16(p, 45); p = put16(p, 2); *p++ = 1; *p++ = 1;                  /* psk_key_exchange_modes */
    p = put16(p, 51); p = put16(p, 2 + 4 + 32); p = put16(p, 4 + 32);        /* key_share */
    p = put16(p, 0x001d); p = put16(p, 32);
    cpy(p, pub, 32); p += 32;
    put16(ext_len, (u32)(p - ext_start));
    u32 body = (u32)(p - msg - 4);
    msg[0] = HS_CLIENT_HELLO;
    msg[1] = (u8)(body >> 16); msg[2] = (u8)(body >> 8); msg[3] = (u8)body;
    sha256_update(&t->transcript, msg, body + 4);
    return send_record(t, CT_HANDSHAKE, msg, body + 4);
}

/* ---- ServerHello -------------------------------------------------------- */

static const u8 HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };

static int process_server_hello(struct tls *t, const u8 *b, u32 n, u8 shared[32])
{
    const u8 *end = b + n;
    if (n < 38) return TLS_EPROTO;
    if (!memcmp(b + 2, HRR_RANDOM, 32)) return TLS_EUNSUPP;   /* HelloRetryRequest: needs another group */
    b += 34;
    u32 sid = *b++;
    if (b + sid + 3 > end) return TLS_EPROTO;
    b += sid;
    t->suite = (u16)(b[0] << 8 | b[1]);
    if (t->suite != SUITE_AES128_GCM && t->suite != SUITE_CHACHA20) return TLS_EUNSUPP;
    b += 3;                              /* cipher suite + compression */
    if (b + 2 > end) return TLS_EPROTO;
    u32 elen = (u32)b[0] << 8 | b[1];
    b += 2;
    if (b + elen > end) return TLS_EPROTO;
    bool version13 = false, have_key = false;
    const u8 *e = b, *eend = b + elen;
    while (e + 4 <= eend) {
        u32 type = (u32)e[0] << 8 | e[1], len = (u32)e[2] << 8 | e[3];
        e += 4;
        if (e + len > eend) return TLS_EPROTO;
        if (type == 43 && len == 2 && e[0] == 3 && e[1] == 4) version13 = true;
        if (type == 51 && len >= 4) {
            u32 group = (u32)e[0] << 8 | e[1], klen = (u32)e[2] << 8 | e[3];
            if (group != 0x001d || klen != 32 || len < 4 + klen) return TLS_EUNSUPP;
            x25519(shared, t->priv, e + 4);
            have_key = true;
        }
        e += len;
    }
    if (!version13) return TLS_EUNSUPP;  /* server only speaks TLS 1.2 or older */
    if (!have_key) return TLS_EPROTO;
    u8 acc = 0;
    for (int i = 0; i < 32; i++) acc |= shared[i];
    return acc ? TLS_OK : TLS_EPROTO;     /* all-zero shared secret: invalid public key */
}

/* ---- the handshake -------------------------------------------------------- */

int tls_handshake(struct tls *t, u32 timeout_ms)
{
    t->timeout = timeout_ms;
    int r = send_client_hello(t);
    if (r) return r;

    u32 n;
    int type = next_handshake(t, &n);
    if (type < 0) return type;
    if (type != HS_SERVER_HELLO) return TLS_EPROTO;
    u8 shared[32];
    r = process_server_hello(t, t->hs + 4, n, shared);
    if (r) return r;
    consume_handshake(t, n, true);

    /* handshake secrets */
    u8 zeros[32], early[32], derived[32], empty[32], th[32], secret[32];
    memset(zeros, 0, 32);
    sha256("", 0, empty);
    hkdf_extract(NULL, 0, zeros, 32, early);
    hkdf_expand_label(early, "derived", empty, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, t->hs_secret);
    transcript_hash(t, th);
    u8 c_hs[32], s_hs[32];
    hkdf_expand_label(t->hs_secret, "c hs traffic", th, 32, c_hs, 32);
    hkdf_expand_label(t->hs_secret, "s hs traffic", th, 32, s_hs, 32);
    set_keys(t, &t->rx, s_hs);
    hkdf_expand_label(t->hs_secret, "derived", empty, 32, derived, 32);
    hkdf_extract(derived, 32, zeros, 32, t->master);
    if (t->rec_pos < t->rec_len) return TLS_EPROTO;   /* plaintext data after ServerHello */

    /* encrypted handshake messages up to the server's Finished */
    bool cert_requested = false;
    for (;;) {
        type = next_handshake(t, &n);
        if (type < 0) return type;
        if (type == HS_FINISHED) break;
        if (type == HS_CERTIFICATE_REQUEST) cert_requested = true;
        else if (type != HS_ENCRYPTED_EXTENSIONS && type != HS_CERTIFICATE && type != HS_CERTIFICATE_VERIFY)
            return TLS_EPROTO;
        /* Certificate and CertificateVerify are hashed into the transcript but
         * not verified: NimbusOS has no certificate store (see tls.h). */
        consume_handshake(t, n, true);
    }
    /* verify the server Finished: HMAC(finished_key, transcript so far) */
    u8 fkey[32], expect[32];
    hkdf_expand_label(s_hs, "finished", NULL, 0, fkey, 32);
    transcript_hash(t, th);
    hmac_sha256(fkey, 32, th, 32, expect);
    if (n != 32 || crypto_memcmp(expect, t->hs + 4, 32)) return TLS_ECRYPT;
    consume_handshake(t, n, true);

    /* application secrets use the transcript through the server Finished */
    transcript_hash(t, th);
    u8 c_ap[32], s_ap[32];
    hkdf_expand_label(t->master, "c ap traffic", th, 32, c_ap, 32);
    hkdf_expand_label(t->master, "s ap traffic", th, 32, s_ap, 32);

    /* client side: (compat) ChangeCipherSpec, empty Certificate if asked, Finished */
    static const u8 ccs = 1;
    r = send_record(t, CT_CCS, &ccs, 1);
    if (r) return r;
    set_keys(t, &t->tx, c_hs);
    if (cert_requested) {
        u8 empty_cert[8] = { HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0 };
        sha256_update(&t->transcript, empty_cert, 8);
        r = send_record(t, CT_HANDSHAKE, empty_cert, 8);
        if (r) return r;
    }
    u8 fin[36];
    hkdf_expand_label(c_hs, "finished", NULL, 0, fkey, 32);
    transcript_hash(t, th);
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    hmac_sha256(fkey, 32, th, 32, fin + 4);
    r = send_record(t, CT_HANDSHAKE, fin, 36);
    if (r) return r;

    set_keys(t, &t->tx, c_ap);
    set_keys(t, &t->rx, s_ap);
    memset(secret, 0, 32);
    memset(t->priv, 0, 32);
    return TLS_OK;
}

/* ---- application data --------------------------------------------------- */

static void key_update(struct dir *d, struct tls *t)
{
    u8 next[32];
    hkdf_expand_label(d->secret, "traffic upd", NULL, 0, next, 32);
    set_keys(t, d, next);
}

int tls_write(struct tls *t, const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 left = len;
    if (t->key_update_requested) {
        u8 ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };     /* update_not_requested */
        int r = send_record(t, CT_HANDSHAKE, ku, 5);
        if (r) return r;
        key_update(&t->tx, t);
        t->key_update_requested = false;
    }
    while (left) {
        u32 n = left > 16384 ? 16384 : left;
        int r = send_record(t, CT_APPDATA, p, n);
        if (r) return r;
        p += n; left -= n;
    }
    return (int)len;
}

int tls_read(struct tls *t, void *buf, u32 max, u32 timeout_ms)
{
    t->timeout = timeout_ms;
    for (;;) {
        if (t->rec_type == CT_APPDATA && t->rec_pos < t->rec_len) {
            u32 n = t->rec_len - t->rec_pos;
            if (n > max) n = max;
            cpy(buf, t->rec + t->rec_pos, n);
            t->rec_pos += n;
            return (int)n;
        }
        if (t->closed) return 0;
        /* post-handshake messages already buffered */
        if (t->rec_type == CT_HANDSHAKE && t->rec_pos < t->rec_len) {
            u32 take = t->rec_len - t->rec_pos;
            if (t->hs_len + take > HS_MAX + 4) return TLS_EPROTO;
            cpy(t->hs + t->hs_len, t->rec + t->rec_pos, take);
            t->hs_len += take;
            t->rec_pos = t->rec_len;
        }
        while (t->hs_len >= 4) {
            u32 n = (u32)t->hs[1] << 16 | (u32)t->hs[2] << 8 | t->hs[3];
            if (t->hs_len < 4 + n) break;
            if (t->hs[0] == HS_KEY_UPDATE && n == 1) {
                key_update(&t->rx, t);
                if (t->hs[4] == 1) t->key_update_requested = true;
            }
            /* NewSessionTicket and anything else: ignored */
            consume_handshake(t, n, false);
        }
        int r = read_record(t);
        if (r <= 0) return r;
    }
}

void tls_close(struct tls *t)
{
    if (!t || !t->tx.on || t->closed) return;
    static const u8 close_notify[2] = { 1, 0 };
    send_record(t, CT_ALERT, close_notify, 2);
}
