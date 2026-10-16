/* NimbusOS - TCP/IP stack API.  IPv4 addresses are host-order u32
 * (a.b.c.d == a<<24 | b<<16 | c<<8 | d).  The blocking calls must run in a
 * worker task, never in the GUI or network task. */
#ifndef NIMBUS_NET_H
#define NIMBUS_NET_H
#include "kernel/types.h"

#define NET_ETIMEDOUT -1
#define NET_EUNREACH  -2
#define NET_ERESET    -3
#define NET_ENOMEM    -4
#define NET_ENODEV    -5
#define NET_EBADURL   -6
#define NET_EPROTO    -7
#define NET_ENOTFOUND -8
#define NET_EDNS      -9
#define NET_ETLS      -10   /* see net_tls_error() for the detail */

struct netif {
    bool present, configured;
    u8 mac[6];
    u32 ip, mask, gw, dns;
    u32 rx_packets, tx_packets, rx_bytes, tx_bytes, dropped;
};
extern struct netif g_net;

void net_init(void);                        /* NIC + network task + DHCP in the background */
void rng_stir(u64 v);                       /* net/rng.c: mix a value into the random pool */
const char *net_strerror(int e);
void ip_to_str(u32 ip, char *out);          /* out >= 16 bytes */
bool ip_from_str(const char *s, u32 *ip);

int  net_dhcp(u32 timeout_ms);
int  net_ping(u32 ip, u16 seq, u32 timeout_ms, u32 *rtt_ms);
int  net_resolve(const char *name, u32 *ip, u32 timeout_ms);   /* also accepts dotted quads */
int  net_resolve_via(const char *name, u32 server, u32 *ip, u32 timeout_ms);

int  tcp_connect(u32 ip, u16 port, u32 timeout_ms);            /* socket id or error */
int  tcp_send(int s, const void *buf, u32 len);
int  tcp_recv(int s, void *buf, u32 max, u32 timeout_ms);       /* bytes, 0 = closed, <0 error */
void tcp_close(int s);

/* HTTP/1.0 GET of an http:// URL (follows up to 3 redirects).  *body is
 * kmalloc'ed; *status gets the HTTP status code. */
const char *net_tls_error(void);            /* detail for NET_ETLS */
const char *http_last_security(void);       /* cipher of the last https request, "" for http */
int  http_get(const char *url, char **body, u32 *len, int *status, char *final_url, int urlmax);
#endif
