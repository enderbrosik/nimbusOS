/*
 * NimbusOS - TLS 1.3 client (RFC 8446), written from scratch.
 *
 * Key exchange X25519; cipher suites TLS_AES_128_GCM_SHA256 and
 * TLS_CHACHA20_POLY1305_SHA256.  The server's certificate is NOT verified:
 * the connection is encrypted and tamper-proof, but it does not prove who
 * the server is.  The transport is a pair of callbacks, so the same code
 * runs over the kernel's TCP stack and over host sockets (tools/tlstest.c).
 */
#ifndef NIMBUS_TLS_H
#define NIMBUS_TLS_H
#include "net/crypto.h"

#define TLS_OK          0
#define TLS_EIO        -1     /* transport failed or timed out */
#define TLS_EPROTO     -2     /* malformed or unexpected message */
#define TLS_EALERT     -3     /* server sent a fatal alert */
#define TLS_EUNSUPP    -4     /* server wants something we do not implement */
#define TLS_ECRYPT     -5     /* record failed authentication */
#define TLS_ENOMEM     -6

struct tls_io {
    void *ctx;
    int (*send)(void *ctx, const void *buf, u32 len);               /* bytes or < 0 */
    int (*recv)(void *ctx, void *buf, u32 max, u32 timeout_ms);     /* bytes, 0 closed, < 0 error */
};

struct tls;
struct tls *tls_new(const struct tls_io *io, const char *server_name);
int  tls_handshake(struct tls *t, u32 timeout_ms);
int  tls_write(struct tls *t, const void *buf, u32 len);
int  tls_read(struct tls *t, void *buf, u32 max, u32 timeout_ms);  /* bytes, 0 = closed */
void tls_close(struct tls *t);                                       /* sends close_notify */
void tls_free(struct tls *t);
const char *tls_cipher_name(struct tls *t);
const char *tls_strerror(int e);
int  tls_last_alert(struct tls *t);                                  /* alert number, -1 if none */

/* Platform hook: fill buf with unpredictable bytes. */
void tls_random_bytes(u8 *buf, u32 n);
#endif
