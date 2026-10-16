/*
 * NimbusOS - cryptography for TLS 1.3, written from scratch.
 *
 *   SHA-256, HMAC-SHA256, HKDF        FIPS 180-4, RFC 2104, RFC 5869
 *   X25519                            RFC 7748
 *   ChaCha20-Poly1305 AEAD            RFC 8439
 *   AES-128-GCM AEAD                  FIPS 197, NIST SP 800-38D
 *
 * Integer arithmetic only (the kernel has no FPU/SSE state).  This file
 * builds both in the kernel and on the host (tools/cryptotest.c).
 */
#ifndef NIMBUS_CRYPTO_H
#define NIMBUS_CRYPTO_H
#ifdef NIMBUS_KERNEL
#include "kernel/types.h"
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* ---- SHA-256 ---- */
struct sha256 { u32 h[8]; u8 buf[64]; u64 len; u32 n; };
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, u8 out[32]);
void sha256(const void *data, size_t len, u8 out[32]);

void hmac_sha256(const u8 *key, size_t klen, const void *data, size_t len, u8 out[32]);
void hkdf_extract(const u8 *salt, size_t slen, const u8 *ikm, size_t ilen, u8 prk[32]);
void hkdf_expand(const u8 prk[32], const u8 *info, size_t ilen, u8 *out, size_t olen);
/* TLS 1.3 HKDF-Expand-Label (RFC 8446 section 7.1) */
void hkdf_expand_label(const u8 secret[32], const char *label, const u8 *ctx, size_t clen, u8 *out, size_t olen);

/* ---- X25519 ---- */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]);      /* scalar * 9 */

/* ---- AEADs: seal appends a 16-byte tag; open returns 0 if the tag matches ---- */
void chacha20_poly1305_seal(const u8 key[32], const u8 nonce[12], const u8 *ad, size_t adlen,
                            const u8 *pt, size_t len, u8 *ct_and_tag);
int  chacha20_poly1305_open(const u8 key[32], const u8 nonce[12], const u8 *ad, size_t adlen,
                            const u8 *ct_and_tag, size_t len_with_tag, u8 *pt);

struct aes128 { u32 rk[44]; };
void aes128_init(struct aes128 *a, const u8 key[16]);
void aes128_encrypt(const struct aes128 *a, const u8 in[16], u8 out[16]);
void aes128_gcm_seal(const u8 key[16], const u8 nonce[12], const u8 *ad, size_t adlen,
                     const u8 *pt, size_t len, u8 *ct_and_tag);
int  aes128_gcm_open(const u8 key[16], const u8 nonce[12], const u8 *ad, size_t adlen,
                     const u8 *ct_and_tag, size_t len_with_tag, u8 *pt);

/* constant-time comparison: 0 if equal */
int crypto_memcmp(const void *a, const void *b, size_t n);
#endif
