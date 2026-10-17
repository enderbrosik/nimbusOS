/*
 * NimbusOS - a small TCP/IP stack.
 *
 *   Ethernet II, ARP (RFC 826), IPv4 (RFC 791, no fragmentation), ICMP echo
 *   (RFC 792), UDP (RFC 768), DHCP client (RFC 2131/2132), DNS stub
 *   resolver (RFC 1035, A records), TCP client (RFC 793: three-way
 *   handshake, in-order receive, retransmission with exponential backoff,
 *   FIN/RST handling; no window scaling or SACK), HTTP/1.0 GET.
 *
 * The "net" kernel task polls the NIC, processes every incoming frame and
 * runs TCP retransmission timers.  Applications call the blocking API from
 * worker tasks; shared state is protected by disabling interrupts, which is
 * enough on this uniprocessor kernel.
 */
#include "net/net.h"
#include "net/tls.h"
#include "drivers/e1000.h"
#include "drivers/pit.h"
#include "kernel/task.h"
#include "kernel/heap.h"
#include "kernel/version.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "arch/x86/io.h"

struct netif g_net;

static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (u8)v; }

#define LOCK() u32 _lf = irq_save()
#define UNLOCK() irq_restore(_lf)

static const u8 BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void ip_to_str(u32 ip, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool ip_from_str(const char *s, u32 *ip)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit(*s)) return false;
        int n = 0;
        while (isdigit(*s)) { n = n * 10 + (*s++ - '0'); if (n > 255) return false; }
        v = v << 8 | (u32)n;
        if (i < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

const char *net_strerror(int e)
{
    switch (e) {
    case NET_ETIMEDOUT: return "timed out";
    case NET_EUNREACH: return "host unreachable";
    case NET_ERESET: return "connection reset";
    case NET_ENOMEM: return "out of memory";
    case NET_ENODEV: return "no network";
    case NET_EBADURL: return "bad URL (use http:// or https://)";
    case NET_ETLS: return net_tls_error()[0] ? net_tls_error() : "secure connection failed";
    case NET_EPROTO: return "protocol error";
    case NET_ENOTFOUND: return "name not found";
    case NET_EDNS: return "DNS server failure";
    default: return "error";
    }
}

static u16 csum(const void *data, u32 len, u32 sum)
{
    const u8 *p = data;
    for (u32 i = 0; i + 1 < len; i += 2) sum += (u32)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (u32)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* ====================================================================
 * Ethernet + ARP
 * ================================================================== */
static u8 frame_out[1600];

static int eth_send(const u8 dst[6], u16 type, const void *payload, u32 len)
{
    if (len > 1500) return NET_EPROTO;
    LOCK();
    memcpy(frame_out, dst, 6);
    memcpy(frame_out + 6, g_net.mac, 6);
    put16(frame_out + 12, type);
    memcpy(frame_out + 14, payload, len);
    int r = e1000_send(frame_out, len + 14);
    if (!r) { g_net.tx_packets++; g_net.tx_bytes += len + 14; }
    UNLOCK();
    return r ? NET_ENODEV : 0;
}

#define ARP_N 16
static struct { u32 ip; u8 mac[6]; bool valid; } arp_cache[ARP_N];
static int arp_next;

static void arp_store(u32 ip, const u8 mac[6])
{
    LOCK();
    int slot = -1;
    for (int i = 0; i < ARP_N; i++) if (arp_cache[i].valid && arp_cache[i].ip == ip) slot = i;
    if (slot < 0) { slot = arp_next; arp_next = (arp_next + 1) % ARP_N; }
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].valid = true;
    UNLOCK();
}

static bool arp_lookup(u32 ip, u8 mac[6])
{
    LOCK();
    for (int i = 0; i < ARP_N; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { memcpy(mac, arp_cache[i].mac, 6); UNLOCK(); return true; }
    UNLOCK();
    return false;
}

static void arp_send(u16 op, const u8 tmac[6], u32 tip)
{
    u8 p[28];
    put16(p, 1); put16(p + 2, 0x0800); p[4] = 6; p[5] = 4; put16(p + 6, op);
    memcpy(p + 8, g_net.mac, 6); put32(p + 14, g_net.ip);
    memcpy(p + 18, tmac, 6); put32(p + 24, tip);
    eth_send(op == 1 ? BCAST : tmac, 0x0806, p, 28);
}

static void arp_input(const u8 *p, u32 len)
{
    if (len < 28 || get16(p) != 1 || get16(p + 2) != 0x0800) return;
    u16 op = get16(p + 6);
    u32 sip = get32(p + 14), tip = get32(p + 24);
    if (sip) arp_store(sip, p + 8);
    if (op == 1 && g_net.configured && tip == g_net.ip) arp_send(2, p + 8, sip);
}

static bool in_net_task(void);

/* Resolve the next hop's MAC; waits (worker tasks only) for an ARP reply. */
static int resolve_mac(u32 dst, u8 mac[6])
{
    if (dst == 0xFFFFFFFF || !g_net.configured) { memcpy(mac, BCAST, 6); return 0; }
    u32 hop = ((dst & g_net.mask) == (g_net.ip & g_net.mask)) ? dst : g_net.gw;
    if (arp_lookup(hop, mac)) return 0;
    if (in_net_task()) return NET_EUNREACH;
    for (int tries = 0; tries < 3; tries++) {
        arp_send(1, (const u8[6]){ 0 }, hop);
        for (int i = 0; i < 50; i++) {
            task_sleep(10);
            if (arp_lookup(hop, mac)) return 0;
        }
    }
    return NET_EUNREACH;
}

/* ====================================================================
 * IPv4 / ICMP
 * ================================================================== */
static u16 ip_id;
static u8 ip_out[1500];

static int ip_send(u32 dst, u8 proto, const void *payload, u32 len)
{
    if (len > 1480) return NET_EPROTO;
    u8 mac[6];
    int r = resolve_mac(dst, mac);
    if (r) return r;
    LOCK();
    u8 *h = ip_out;
    memset(h, 0, 20);
    h[0] = 0x45;
    put16(h + 2, (u16)(20 + len));
    put16(h + 4, ip_id++);
    h[8] = 64;
    h[9] = proto;
    put32(h + 12, g_net.configured ? g_net.ip : 0);
    put32(h + 16, dst);
    put16(h + 10, csum(h, 20, 0));
    memcpy(h + 20, payload, len);
    r = eth_send(mac, 0x0800, h, 20 + len);
    UNLOCK();
    return r;
}

static struct { volatile bool waiting, got; u16 id, seq; u64 sent; u32 rtt; } ping_state;

static void icmp_input(u32 src, const u8 *p, u32 len)
{
    if (len < 8 || csum(p, len, 0) != 0) return;
    if (p[0] == 8) {                       /* echo request -> reply */
        u8 *r = kmalloc(len);
        if (!r) return;
        memcpy(r, p, len);
        r[0] = 0;
        r[2] = r[3] = 0;
        put16(r + 2, csum(r, len, 0));
        ip_send(src, 1, r, len);
        kfree(r);
    } else if (p[0] == 0 && ping_state.waiting && get16(p + 4) == ping_state.id && get16(p + 6) == ping_state.seq) {
        ping_state.rtt = (u32)(timer_ticks() - ping_state.sent);
        ping_state.got = true;
    }
}

int net_ping(u32 ip, u16 seq, u32 timeout_ms, u32 *rtt)
{
    if (!g_net.present) return NET_ENODEV;
    u8 pkt[8 + 32];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 8;
    ping_state.id = 0x4E42;
    ping_state.seq = seq;
    put16(pkt + 4, ping_state.id);
    put16(pkt + 6, seq);
    for (int i = 0; i < 32; i++) pkt[8 + i] = 'a' + i % 26;
    put16(pkt + 2, csum(pkt, sizeof pkt, 0));
    ping_state.got = false;
    ping_state.waiting = true;
    ping_state.sent = timer_ticks();
    int r = ip_send(ip, 1, pkt, sizeof pkt);
    if (r) { ping_state.waiting = false; return r; }
    u64 end = timer_ticks() + timeout_ms;
    while (!ping_state.got && timer_ticks() < end) task_sleep(2);
    ping_state.waiting = false;
    if (!ping_state.got) return NET_ETIMEDOUT;
    *rtt = ping_state.rtt;
    return 0;
}

/* ====================================================================
 * UDP (a few sockets with a small datagram queue each)
 * ================================================================== */
#define UDP_N 8
#define UDP_Q 4
struct dgram { u32 src; u16 sport; u16 len; u8 data[1472]; };
static struct { bool used; u16 port; struct dgram *q[UDP_Q]; int qh, qn; } udp[UDP_N];

static int udp_open(u16 port)
{
    LOCK();
    for (int i = 0; i < UDP_N; i++)
        if (!udp[i].used) { memset(&udp[i], 0, sizeof udp[i]); udp[i].used = true; udp[i].port = port; UNLOCK(); return i; }
    UNLOCK();
    return NET_ENOMEM;
}

static void udp_close(int s)
{
    LOCK();
    for (int i = 0; i < udp[s].qn; i++) kfree(udp[s].q[(udp[s].qh + i) % UDP_Q]);
    udp[s].used = false;
    UNLOCK();
}

static int udp_sendto(int s, u32 dst, u16 dport, const void *data, u32 len)
{
    u8 *p = kmalloc(8 + len);
    if (!p) return NET_ENOMEM;
    put16(p, udp[s].port);
    put16(p + 2, dport);
    put16(p + 4, (u16)(8 + len));
    put16(p + 6, 0);                     /* checksum optional in IPv4 */
    memcpy(p + 8, data, len);
    int r = ip_send(dst, 17, p, 8 + len);
    kfree(p);
    return r;
}

static int udp_recv(int s, void *buf, u32 max, u32 *src, u32 timeout_ms)
{
    u64 end = timer_ticks() + timeout_ms;
    for (;;) {
        LOCK();
        if (udp[s].qn) {
            struct dgram *d = udp[s].q[udp[s].qh];
            udp[s].qh = (udp[s].qh + 1) % UDP_Q;
            udp[s].qn--;
            UNLOCK();
            u32 n = MIN(max, d->len);
            memcpy(buf, d->data, n);
            if (src) *src = d->src;
            kfree(d);
            return (int)n;
        }
        UNLOCK();
        if (timer_ticks() >= end) return NET_ETIMEDOUT;
        task_sleep(5);
    }
}

static void udp_input(u32 src, const u8 *p, u32 len)
{
    if (len < 8) return;
    u16 sport = get16(p), dport = get16(p + 2), ulen = get16(p + 4);
    if (ulen < 8 || ulen > len) return;
    for (int i = 0; i < UDP_N; i++) {
        if (!udp[i].used || udp[i].port != dport || udp[i].qn == UDP_Q) continue;
        struct dgram *d = kmalloc(sizeof *d);
        if (!d) return;
        d->src = src;
        d->sport = sport;
        d->len = (u16)MIN((u32)ulen - 8, sizeof d->data);
        memcpy(d->data, p + 8, d->len);
        udp[i].q[(udp[i].qh + udp[i].qn) % UDP_Q] = d;
        udp[i].qn++;
        return;
    }
}

static u16 ephemeral(void) { return (u16)(49152 + rand_range(16000)); }

/* ====================================================================
 * DHCP client
 * ================================================================== */
static int dhcp_build(u8 *p, u8 type, u32 xid, u32 req_ip, u32 server)
{
    memset(p, 0, 300);
    p[0] = 1; p[1] = 1; p[2] = 6;
    put32(p + 4, xid);
    put16(p + 10, 0x8000);              /* broadcast reply */
    memcpy(p + 28, g_net.mac, 6);
    put32(p + 236, 0x63825363);          /* magic cookie */
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = type;
    if (req_ip) { p[o++] = 50; p[o++] = 4; put32(p + o, req_ip); o += 4; }
    if (server) { p[o++] = 54; p[o++] = 4; put32(p + o, server); o += 4; }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;   /* mask, router, dns */
    p[o++] = 12; p[o++] = 7; memcpy(p + o, "nimbus1", 7); o += 7;
    p[o++] = 255;
    return MAX(o, 300);
}

static u8 dhcp_option(const u8 *p, u32 len, u8 code, u8 *out, u32 outlen)
{
    for (u32 o = 240; o + 1 < len && p[o] != 255;) {
        if (p[o] == 0) { o++; continue; }
        u8 c = p[o], l = p[o + 1];
        if (c == code) { memcpy(out, p + o + 2, MIN(l, outlen)); return l; }
        o += 2 + l;
    }
    return 0;
}

int net_dhcp(u32 timeout_ms)
{
    if (!g_net.present) return NET_ENODEV;
    int s = udp_open(68);
    if (s < 0) return s;
    u8 *pkt = kmalloc(600);
    u32 xid = rand32();
    int r = NET_ETIMEDOUT;
    u32 offered = 0, server = 0;
    g_net.configured = false;
    for (int stage = 0; stage < 2 && timer_ticks() < timer_ticks() + timeout_ms; stage++) {
        int n = dhcp_build(pkt, stage == 0 ? 1 : 3, xid, offered, server);   /* DISCOVER / REQUEST */
        udp_sendto(s, 0xFFFFFFFF, 67, pkt, n);
        u64 end = timer_ticks() + timeout_ms / 2;
        bool ok = false;
        while (timer_ticks() < end) {
            int len = udp_recv(s, pkt, 600, NULL, 500);
            if (len < 240 || get32(pkt + 4) != xid || pkt[0] != 2) continue;
            u8 type = 0;
            dhcp_option(pkt, len, 53, &type, 1);
            if (stage == 0 && type == 2) {                  /* OFFER */
                offered = get32(pkt + 16);
                u8 sid[4] = { 0 };
                dhcp_option(pkt, len, 54, sid, 4);
                server = get32(sid);
                ok = true;
                break;
            }
            if (stage == 1 && type == 5) {                  /* ACK */
                u8 v[4] = { 0 };
                g_net.ip = get32(pkt + 16);
                if (dhcp_option(pkt, len, 1, v, 4)) g_net.mask = get32(v); else g_net.mask = 0xFFFFFF00;
                if (dhcp_option(pkt, len, 3, v, 4)) g_net.gw = get32(v);
                if (dhcp_option(pkt, len, 6, v, 4)) g_net.dns = get32(v);
                g_net.configured = true;
                ok = true;
                r = 0;
                break;
            }
            if (type == 6) { r = NET_EPROTO; break; }        /* NAK */
        }
        if (!ok) break;
    }
    kfree(pkt);
    udp_close(s);
    if (!r) {
        char a[16], m[16], g[16], d[16];
        ip_to_str(g_net.ip, a); ip_to_str(g_net.mask, m); ip_to_str(g_net.gw, g); ip_to_str(g_net.dns, d);
        kprintf("[net ] DHCP: address %s mask %s gateway %s dns %s\n", a, m, g, d);
    } else {
        kprintf("[net ] DHCP failed: %s\n", net_strerror(r));
    }
    return r;
}

/* ====================================================================
 * DNS
 * ================================================================== */
static int dns_skip_name(const u8 *p, int len, int o)
{
    while (o < len) {
        if (p[o] == 0) return o + 1;
        if ((p[o] & 0xC0) == 0xC0) return o + 2;
        o += p[o] + 1;
    }
    return -1;
}

int net_resolve(const char *name, u32 *ip, u32 timeout_ms)
{
    return net_resolve_via(name, g_net.dns, ip, timeout_ms);
}

int net_resolve_via(const char *name, u32 server, u32 *ip, u32 timeout_ms)
{
    if (ip_from_str(name, ip)) return 0;
    if (!g_net.configured) return NET_ENODEV;
    int s = udp_open(ephemeral());
    if (s < 0) return s;
    u8 q[300], a[600];
    u16 id = (u16)rand32();
    memset(q, 0, 12);
    put16(q, id);
    put16(q + 2, 0x0100);                /* recursion desired */
    put16(q + 4, 1);
    int o = 12;
    for (const char *p = name; *p && o < 260;) {
        const char *dot = strchr(p, '.');
        int l = dot ? (int)(dot - p) : (int)strlen(p);
        if (l == 0 || l > 63) { udp_close(s); return NET_EBADURL; }
        q[o++] = (u8)l;
        memcpy(q + o, p, l);
        o += l;
        p += l + (dot ? 1 : 0);
    }
    q[o++] = 0;
    put16(q + o, 1); put16(q + o + 2, 1);    /* type A, class IN */
    o += 4;
    int r = NET_ETIMEDOUT;
    for (int tries = 0; tries < 3 && r == NET_ETIMEDOUT; tries++) {
        udp_sendto(s, server, 53, q, o);
        u64 end = timer_ticks() + timeout_ms / 3;
        while (timer_ticks() < end) {
            int len = udp_recv(s, a, sizeof a, NULL, (u32)(end - timer_ticks()) + 1);
            if (len < 12 || get16(a) != id) continue;
            int an = get16(a + 6), off = dns_skip_name(a, len, 12) + 4;
            int rcode = get16(a + 2) & 0xF;
            if (rcode == 3) { r = NET_ENOTFOUND; break; }
            if (off < 0 || rcode != 0) { r = NET_EDNS; break; }
            for (int i = 0; i < an && off > 0 && off + 10 <= len; i++) {
                off = dns_skip_name(a, len, off);
                if (off < 0 || off + 10 > len) break;
                u16 type = get16(a + off), rdlen = get16(a + off + 8);
                if (type == 1 && rdlen == 4 && off + 14 <= len) { *ip = get32(a + off + 10); r = 0; break; }
                off += 10 + rdlen;
            }
            if (r != 0) r = NET_ENOTFOUND;
            break;
        }
    }
    udp_close(s);
    return r;
}

/* ====================================================================
 * TCP (client side)
 * ================================================================== */
enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT };
#define TCP_N 8
#define RXBUF 65536
#define TXBUF 16384
#define MSS 1400

struct tcb {
    bool used;
    int state;
    u32 rip;
    u16 lport, rport;
    u32 snd_una, snd_nxt, rcv_nxt, iss;
    u16 snd_wnd;
    u8 *rx; u32 rx_head, rx_len;          /* ring of received bytes */
    u8 *tx; u32 tx_len;                   /* unacknowledged bytes from snd_una */
    bool fin_rcvd, fin_sent, reset;
    u64 rto_at; u32 rto;
};
static struct tcb tcbs[TCP_N];

static void tcp_output(struct tcb *t, u8 flags, const u8 *data, u32 len, u32 seq)
{
    u8 seg[20 + MSS];
    memset(seg, 0, 20);
    put16(seg, t->lport);
    put16(seg + 2, t->rport);
    put32(seg + 4, seq);
    put32(seg + 8, (flags & 0x10) ? t->rcv_nxt : 0);
    seg[12] = 5 << 4;
    seg[13] = flags;
    u32 space = RXBUF - t->rx_len;
    put16(seg + 14, (u16)MIN(space, 65535u));
    if (len) memcpy(seg + 20, data, len);
    /* checksum over the pseudo header + segment */
    u8 ph[12];
    put32(ph, g_net.ip); put32(ph + 4, t->rip); ph[8] = 0; ph[9] = 6; put16(ph + 10, (u16)(20 + len));
    u32 sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (u32)(ph[i] << 8 | ph[i + 1]);
    put16(seg + 16, csum(seg, 20 + len, sum));
    ip_send(t->rip, 6, seg, 20 + len);
}

/* (re)send everything unacknowledged, a segment at a time */
static void tcp_flush(struct tcb *t)
{
    u32 inflight = t->snd_nxt - t->snd_una;
    while (inflight < t->tx_len && inflight < t->snd_wnd) {
        u32 n = MIN(MIN((u32)MSS, t->tx_len - inflight), (u32)t->snd_wnd - inflight);
        tcp_output(t, 0x18, t->tx + inflight, n, t->snd_una + inflight);   /* PSH|ACK */
        inflight += n;
        t->snd_nxt = t->snd_una + inflight;
    }
    if (t->snd_nxt != t->snd_una || t->state == TCP_SYN_SENT) t->rto_at = timer_ticks() + t->rto;
}

static void tcp_input(u32 src, const u8 *p, u32 len)
{
    if (len < 20) return;
    u16 sport = get16(p), dport = get16(p + 2);
    u32 seq = get32(p + 4), ack = get32(p + 8);
    u32 hl = (p[12] >> 4) * 4;
    u8 fl = p[13];
    if (hl < 20 || hl > len) return;
    const u8 *data = p + hl;
    u32 dlen = len - hl;
    struct tcb *t = NULL;
    for (int i = 0; i < TCP_N; i++)
        if (tcbs[i].used && tcbs[i].lport == dport && tcbs[i].rport == sport && tcbs[i].rip == src) t = &tcbs[i];
    if (!t) return;                       /* no listener support: silently drop */
    if (fl & 0x04) { t->reset = true; t->state = TCP_CLOSED; return; }   /* RST */
    t->snd_wnd = get16(p + 14);
    if (t->state == TCP_SYN_SENT) {
        if ((fl & 0x12) == 0x12 && ack == t->iss + 1) {    /* SYN+ACK */
            t->rcv_nxt = seq + 1;
            t->snd_una = t->snd_nxt = ack;
            t->state = TCP_ESTABLISHED;
            t->rto = 500;
            tcp_output(t, 0x10, NULL, 0, t->snd_nxt);
        }
        return;
    }
    if (fl & 0x10) {                      /* ACK: release acknowledged bytes */
        u32 acked = ack - t->snd_una;
        if (acked > 0 && acked <= t->snd_nxt - t->snd_una + (t->fin_sent ? 1 : 0)) {
            u32 n = MIN(acked, t->tx_len);
            memmove(t->tx, t->tx + n, t->tx_len - n);
            t->tx_len -= n;
            t->snd_una = ack;
            t->rto = 500;
            if (t->snd_una == t->snd_nxt) t->rto_at = 0;
            if (t->fin_sent && t->snd_una == t->snd_nxt) {
                if (t->state == TCP_FIN_WAIT1) t->state = TCP_FIN_WAIT2;
                else if (t->state == TCP_LAST_ACK) t->state = TCP_CLOSED;
            }
        }
    }
    bool need_ack = false;
    if (dlen && seq == t->rcv_nxt) {      /* in-order data only */
        u32 n = MIN(dlen, RXBUF - t->rx_len);
        for (u32 i = 0; i < n; i++) t->rx[(t->rx_head + t->rx_len + i) % RXBUF] = data[i];
        t->rx_len += n;
        t->rcv_nxt += n;
        need_ack = true;
    } else if (dlen) need_ack = true;     /* duplicate / out of order: re-ACK */
    if ((fl & 0x01) && seq + dlen == t->rcv_nxt && !t->fin_rcvd) {   /* FIN */
        t->fin_rcvd = true;
        t->rcv_nxt++;
        need_ack = true;
        if (t->state == TCP_ESTABLISHED) t->state = TCP_CLOSE_WAIT;
        else if (t->state == TCP_FIN_WAIT1 || t->state == TCP_FIN_WAIT2) t->state = TCP_TIME_WAIT;
    }
    if (need_ack) tcp_output(t, 0x10, NULL, 0, t->snd_nxt);
    tcp_flush(t);
}

static void tcp_timers(void)
{
    u64 now = timer_ticks();
    for (int i = 0; i < TCP_N; i++) {
        struct tcb *t = &tcbs[i];
        if (!t->used || !t->rto_at || now < t->rto_at) continue;
        t->rto = MIN(t->rto * 2, 8000u);
        if (t->state == TCP_SYN_SENT) {
            tcp_output(t, 0x02, NULL, 0, t->iss);
            t->rto_at = now + t->rto;
        } else {
            t->snd_nxt = t->snd_una;          /* go back and resend */
            if (t->fin_sent && t->tx_len == 0) tcp_output(t, 0x11, NULL, 0, t->snd_una);
            tcp_flush(t);
            t->rto_at = now + t->rto;
        }
    }
}

int tcp_connect(u32 ip, u16 port, u32 timeout_ms)
{
    if (!g_net.configured) return NET_ENODEV;
    u8 mac[6];
    int r = resolve_mac(ip, mac);           /* make sure the next hop is in the ARP cache */
    if (r) return r;
    int s = -1;
    LOCK();
    for (int i = 0; i < TCP_N && s < 0; i++) if (!tcbs[i].used) s = i;
    if (s < 0) { UNLOCK(); return NET_ENOMEM; }
    struct tcb *t = &tcbs[s];
    memset(t, 0, sizeof *t);
    t->used = true;
    UNLOCK();
    t->rx = kmalloc(RXBUF);
    t->tx = kmalloc(TXBUF);
    if (!t->rx || !t->tx) { tcp_close(s); return NET_ENOMEM; }
    _lf = irq_save();
    t->rip = ip;
    t->rport = port;
    t->lport = ephemeral();
    t->iss = rand32();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->snd_wnd = 1024;
    t->rto = 1000;
    t->state = TCP_SYN_SENT;
    tcp_output(t, 0x02, NULL, 0, t->iss);     /* SYN */
    t->rto_at = timer_ticks() + t->rto;
    UNLOCK();
    u64 end = timer_ticks() + timeout_ms;
    while (t->state == TCP_SYN_SENT && !t->reset && timer_ticks() < end) task_sleep(5);
    if (t->state != TCP_ESTABLISHED) {
        int e = t->reset ? NET_ERESET : NET_ETIMEDOUT;
        tcp_close(s);
        return e;
    }
    return s;
}

int tcp_send(int s, const void *buf, u32 len)
{
    struct tcb *t = &tcbs[s];
    const u8 *p = buf;
    u32 sent = 0;
    while (sent < len) {
        if (t->reset || t->state == TCP_CLOSED) return NET_ERESET;
        LOCK();
        u32 n = MIN(len - sent, TXBUF - t->tx_len);
        memcpy(t->tx + t->tx_len, p + sent, n);
        t->tx_len += n;
        sent += n;
        tcp_flush(t);
        UNLOCK();
        if (sent < len) task_sleep(5);
    }
    return (int)sent;
}

int tcp_recv(int s, void *buf, u32 max, u32 timeout_ms)
{
    struct tcb *t = &tcbs[s];
    u64 end = timer_ticks() + timeout_ms;
    for (;;) {
        LOCK();
        if (t->rx_len) {
            u32 n = MIN(max, t->rx_len);
            bool was_full = t->rx_len > RXBUF / 2;
            for (u32 i = 0; i < n; i++) ((u8 *)buf)[i] = t->rx[(t->rx_head + i) % RXBUF];
            t->rx_head = (t->rx_head + n) % RXBUF;
            t->rx_len -= n;
            if (was_full) tcp_output(t, 0x10, NULL, 0, t->snd_nxt);   /* window update */
            UNLOCK();
            return (int)n;
        }
        bool closed = t->fin_rcvd || t->state == TCP_CLOSED;
        bool reset = t->reset;
        UNLOCK();
        if (reset) return NET_ERESET;
        if (closed) return 0;
        if (timer_ticks() >= end) return NET_ETIMEDOUT;
        task_sleep(5);
    }
}

void tcp_close(int s)
{
    if (s < 0 || s >= TCP_N) return;
    struct tcb *t = &tcbs[s];
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        LOCK();
        t->state = t->state == TCP_ESTABLISHED ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
        t->fin_sent = true;
        tcp_output(t, 0x11, NULL, 0, t->snd_nxt);     /* FIN|ACK */
        t->snd_nxt++;
        t->rto_at = timer_ticks() + t->rto;
        UNLOCK();
        u64 end = timer_ticks() + 1000;              /* brief wait for the peer's ACK */
        while (t->snd_una != t->snd_nxt && !t->reset && timer_ticks() < end) task_sleep(5);
    }
    LOCK();
    kfree(t->rx);
    kfree(t->tx);
    t->rx = t->tx = NULL;
    t->used = false;
    UNLOCK();
}

/* ====================================================================
 * input path + network task
 * ================================================================== */
static struct task *net_task_ptr;
static bool in_net_task(void) { return task_current() == net_task_ptr; }

static void ip_input(const u8 *p, u32 len)
{
    if (len < 20 || (p[0] >> 4) != 4) return;
    u32 hl = (p[0] & 15) * 4, tot = get16(p + 2);
    if (hl < 20 || tot > len || tot < hl || csum(p, hl, 0) != 0) return;
    if ((get16(p + 6) & 0x3FFF) != 0) return;            /* fragments: not supported */
    u32 src = get32(p + 12), dst = get32(p + 16);
    if (g_net.configured && dst != g_net.ip && dst != 0xFFFFFFFF && (dst | g_net.mask) != 0xFFFFFFFF) return;
    switch (p[9]) {
    case 1: icmp_input(src, p + hl, tot - hl); break;
    case 6: tcp_input(src, p + hl, tot - hl); break;
    case 17: udp_input(src, p + hl, tot - hl); break;
    }
}

static void net_task(void *arg)
{
    static u8 frame[2048];
    for (;;) {
        int n, budget = 64;
        while (budget-- && (n = e1000_poll(frame, sizeof frame)) > 14) {
            rng_stir((u64)n << 32 | frame[n - 1]);     /* packet timing feeds the TLS random pool */
            u32 f = irq_save();
            g_net.rx_packets++;
            g_net.rx_bytes += n;
            u16 type = get16(frame + 12);
            if (type == 0x0806) arp_input(frame + 14, n - 14);
            else if (type == 0x0800) {
                if (g_net.configured) arp_store(get32(frame + 14 + 12), frame + 6);   /* learn sender */
                ip_input(frame + 14, n - 14);
            }
            irq_restore(f);
        }
        u32 f = irq_save();
        tcp_timers();
        irq_restore(f);
        task_sleep(2);
    }
}

static void dhcp_task(void *arg)
{
    task_sleep(200);
    net_dhcp(6000);
}

void net_init(void)
{
    if (!e1000_init()) return;
    g_net.present = true;
    e1000_mac(g_net.mac);
    net_task_ptr = task_create("net", net_task, NULL);
    task_create("dhcp", dhcp_task, NULL);
}

/* ====================================================================
 * HTTP
 * ================================================================== */
/* "http://host[:port]/path" or "https://host[:port]/path" */
static int parse_url(const char *url, bool *tls, char *host, int hostmax, u16 *port, char *path, int pathmax)
{
    const char *h;
    if (!strncmp(url, "http://", 7)) { *tls = false; h = url + 7; *port = 80; }
    else if (!strncmp(url, "https://", 8)) { *tls = true; h = url + 8; *port = 443; }
    else return NET_EBADURL;
    const char *slash = strchr(h, '/');
    int hl = slash ? (int)(slash - h) : (int)strlen(h);
    if (hl <= 0 || hl >= hostmax) return NET_EBADURL;
    memcpy(host, h, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; *port = (u16)atoi(colon + 1); }
    if (!host[0]) return NET_EBADURL;
    strlcpy(path, slash ? slash : "/", pathmax);
    return 0;
}

/* A connection: plain TCP, or TLS on top of it. */
struct conn { int sock; struct tls *tls; };

static int tcp_io_send(void *ctx, const void *buf, u32 len) { return tcp_send(((struct conn *)ctx)->sock, buf, len); }
static int tcp_io_recv(void *ctx, void *buf, u32 max, u32 timeout)
{
    return tcp_recv(((struct conn *)ctx)->sock, buf, max, timeout);
}

static char last_tls_error[96], last_tls_info[64];
const char *net_tls_error(void) { return last_tls_error; }
const char *http_last_security(void) { return last_tls_info; }

static int conn_open(struct conn *c, u32 ip, u16 port, bool use_tls, const char *host)
{
    c->tls = NULL;
    c->sock = tcp_connect(ip, port, 6000);
    if (c->sock < 0) return c->sock;
    if (!use_tls) return 0;
    struct tls_io io = { c, tcp_io_send, tcp_io_recv };
    c->tls = tls_new(&io, host);
    if (!c->tls) { tcp_close(c->sock); return NET_ENOMEM; }
    int r = tls_handshake(c->tls, 10000);
    if (r) {
        if (r == TLS_EALERT) snprintf(last_tls_error, sizeof last_tls_error, "%s (alert %d)", tls_strerror(r), tls_last_alert(c->tls));
        else strlcpy(last_tls_error, tls_strerror(r), sizeof last_tls_error);
        klog("[tls ] handshake with %s failed: %s\n", host, last_tls_error);
        tls_free(c->tls);
        tcp_close(c->sock);
        return NET_ETLS;
    }
    strlcpy(last_tls_info, tls_cipher_name(c->tls), sizeof last_tls_info);
    klog("[tls ] connected to %s: %s (certificate not verified)\n", host, last_tls_info);
    return 0;
}

static int conn_send(struct conn *c, const void *buf, u32 len)
{
    return c->tls ? tls_write(c->tls, buf, len) : tcp_send(c->sock, buf, len);
}

static int conn_recv(struct conn *c, void *buf, u32 max, u32 timeout)
{
    if (!c->tls) return tcp_recv(c->sock, buf, max, timeout);
    int r = tls_read(c->tls, buf, max, timeout);
    return r < 0 && r != TLS_EIO ? NET_ETLS : r;
}

static void conn_close(struct conn *c)
{
    if (c->tls) { tls_close(c->tls); tls_free(c->tls); }
    tcp_close(c->sock);
}

/* Find header `name` (case-insensitive) between buf and hdr_end; returns
 * a pointer to its value (leading spaces skipped), NULL if absent. */
static char *find_header(char *buf, char *hdr_end, const char *name)
{
    u32 nl = strlen(name);
    for (char *l = strstr(buf, "\r\n"); l && l < hdr_end; l = strstr(l, "\r\n")) {
        l += 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            char *v = l + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
    }
    return NULL;
}

/* Decode a "Transfer-Encoding: chunked" body in place; returns its length. */
static u32 dechunk(char *body, u32 len)
{
    u32 in = 0, out = 0;
    while (in < len) {
        u32 size = 0;
        bool digits = false;
        while (in < len) {
            char ch = body[in];
            int d = ch >= '0' && ch <= '9' ? ch - '0' : (ch | 32) >= 'a' && (ch | 32) <= 'f' ? (ch | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            size = size * 16 + d;
            digits = true;
            in++;
        }
        char *eol = strstr(body + in, "\r\n");      /* skip chunk extensions */
        if (!digits || !eol) break;
        in = (u32)(eol - body) + 2;
        if (size == 0) break;
        if (size > len - in) size = len - in;      /* truncated: keep what we have */
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;                            /* data + CRLF */
    }
    body[out] = 0;
    return out;
}

/* Resolve a Location header against the URL it came from. */
static void resolve_location(const char *loc, bool tls, const char *host, u16 port, const char *path,
                             char *out, int max)
{
    const char *scheme = tls ? "https" : "http";
    u16 def = tls ? 443 : 80;
    char hostport[160];
    if (port == def) strlcpy(hostport, host, sizeof hostport);
    else snprintf(hostport, sizeof hostport, "%s:%u", host, port);
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) strlcpy(out, loc, max);
    else if (loc[0] == '/' && loc[1] == '/') snprintf(out, max, "%s:%s", scheme, loc);
    else if (loc[0] == '/') snprintf(out, max, "%s://%s%s", scheme, hostport, loc);
    else {
        char dir[384];
        strlcpy(dir, path, sizeof dir);
        char *q = strchr(dir, '?');
        if (q) *q = 0;
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0;
        snprintf(out, max, "%s://%s%s%s", scheme, hostport, dir, loc);
    }
}

int http_get(const char *url_in, char **body, u32 *blen, int *status, char *final_url, int urlmax)
{
    return http_request(url_in, NULL, body, blen, status, final_url, urlmax);
}

int http_request(const char *url_in, const char *post, char **body, u32 *blen, int *status, char *final_url, int urlmax)
{
    char url[512];
    strlcpy(url, url_in, sizeof url);
    last_tls_info[0] = 0;
    for (int redirects = 0; redirects < 6; redirects++) {
        char host[128], path[384];
        u16 port;
        bool use_tls;
        int r = parse_url(url, &use_tls, host, sizeof host, &port, path, sizeof path);
        if (r) return r;
        u32 ip;
        r = net_resolve(host, &ip, 5000);
        if (r) return r;
        struct conn c;
        r = conn_open(&c, ip, port, use_tls, host);
        if (r) return r;
        char req[1024], extra[96] = "";
        if (post) snprintf(extra, sizeof extra, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n", (u32)strlen(post));
        int n = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (compatible; NimbusOS/" NIMBUS_VERSION "; text browser)\r\n"
            "Accept: text/html, text/plain, */*\r\nAccept-Language: en-US,en;q=0.8\r\n%s"
            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", post ? "POST" : "GET", path, host, extra);
        if (conn_send(&c, req, n) < 0 || (post && conn_send(&c, post, strlen(post)) < 0)) { conn_close(&c); return NET_ERESET; }
        u32 cap = 16384, len = 0;
        char *buf = kmalloc(cap + 1);
        if (!buf) { conn_close(&c); return NET_ENOMEM; }
        int got = 0;
        for (;;) {
            if (len == cap) {
                if (cap >= 4 * 1024 * 1024) break;
                cap *= 2;
                char *nb = krealloc(buf, cap + 1);
                if (!nb) break;
                buf = nb;
            }
            got = conn_recv(&c, buf + len, cap - len, 10000);
            if (got <= 0) break;
            len += got;
        }
        conn_close(&c);
        buf[len] = 0;
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (strncmp(buf, "HTTP/1.", 7) || !hdr_end) {
            kfree(buf);
            return got == NET_ETLS ? NET_ETLS : NET_EPROTO;
        }
        int code = atoi(buf + 9);
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            char *loc = find_header(buf, hdr_end, "Location");
            if (loc) {
                char *e = strstr(loc, "\r\n");
                if (e) *e = 0;
                char next[512];
                resolve_location(loc, use_tls, host, port, path, next, sizeof next);
                klog("[http] %d redirect to %s\n", code, next);
                strlcpy(url, next, sizeof url);
                post = NULL;                                    /* the redirect target is fetched with GET */
                kfree(buf);
                continue;
            }
        }
        char *te = find_header(buf, hdr_end, "Transfer-Encoding");
        bool chunked = te && !strncasecmp(te, "chunked", 7);
        u32 hlen = (u32)(hdr_end + 4 - buf);
        memmove(buf, buf + hlen, len - hlen + 1);
        len -= hlen;
        if (chunked) len = dechunk(buf, len);
        *body = buf;
        *blen = len;
        *status = code;
        if (final_url) strlcpy(final_url, url, urlmax);
        return 0;
    }
    return NET_EPROTO;
}
