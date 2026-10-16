/*
 * NimbusOS - host test for net/tls.c: connects to a TLS 1.3 server over a
 * normal socket, does the handshake, sends an HTTP GET and prints the reply.
 *
 *   tlstest <host> <port> [path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include "net/tls.h"

void tls_random_bytes(u8 *buf, u32 n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, buf, n) != (ssize_t)n) { perror("urandom"); exit(1); }
    close(fd);
}

static int sock_send(void *ctx, const void *buf, u32 len) { return (int)send(*(int *)ctx, buf, len, 0); }

static int sock_recv(void *ctx, void *buf, u32 max, u32 timeout_ms)
{
    struct pollfd p = { *(int *)ctx, POLLIN, 0 };
    if (poll(&p, 1, (int)timeout_ms) <= 0) return -1;
    return (int)recv(*(int *)ctx, buf, max, 0);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: tlstest <host> <port> [path]\n"); return 2; }
    const char *host = argv[1], *path = argc > 3 ? argv[3] : "/";
    struct addrinfo hints = { 0 }, *ai;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, argv[2], &hints, &ai)) { fprintf(stderr, "cannot resolve %s\n", host); return 1; }
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) { perror("connect"); return 1; }
    struct tls_io io = { &fd, sock_send, sock_recv };
    struct tls *t = tls_new(&io, host);
    int r = tls_handshake(t, 5000);
    if (r) { printf("handshake failed: %s (alert %d)\n", tls_strerror(r), tls_last_alert(t)); return 1; }
    printf("handshake ok: %s\n", tls_cipher_name(t));
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    tls_write(t, req, n);
    char buf[4096];
    long total = 0;
    while ((r = tls_read(t, buf, sizeof buf, 5000)) > 0) {
        if (total < 600) fwrite(buf, 1, r < 600 - total ? r : 600 - total, stdout);
        total += r;
    }
    printf("\n[received %ld bytes, end: %s]\n", total, r == 0 ? "clean close" : tls_strerror(r));
    tls_close(t);
    tls_free(t);
    close(fd);
    return r == 0 && total > 0 ? 0 : 1;
}
