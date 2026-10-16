/*
 * NimbusOS - network commands as worker tasks.
 *
 * The Terminal (GUI task) starts a job; a kernel task runs the blocking
 * network calls and queues output lines; the Terminal drains them on a
 * timer.  The job is freed by whichever side finishes last, so closing the
 * Terminal while a ping is running is safe.  Files are never written from
 * the worker (the file system is only used from the GUI task): downloads are
 * handed back to the caller to save.
 */
#include "net/netjob.h"
#include "net/net.h"
#include "kernel/task.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "drivers/pit.h"
#include "arch/x86/io.h"

#define QN 256
struct netjob {
    int argc;
    char args[8][192];
    char *lines[QN];
    bool err[QN];
    int qh, qn;
    volatile bool done, cancel, released;
    /* downloads / fetches */
    char *body;
    u32 len;
    int status, error;
    char name[64], final_url[256];
    bool is_fetch;
};

static void say(struct netjob *j, bool error, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    klog("[njob] %s\n", buf);
    char *l = kmalloc(strlen(buf) + 1);
    if (!l) return;
    strcpy(l, buf);
    u32 f = irq_save();
    if (j->qn == QN) { kfree(l); irq_restore(f); return; }
    j->lines[(j->qh + j->qn) % QN] = l;
    j->err[(j->qh + j->qn) % QN] = error;
    j->qn++;
    irq_restore(f);
}

static void free_job(struct netjob *j)
{
    while (j->qn) { kfree(j->lines[j->qh]); j->qh = (j->qh + 1) % QN; j->qn--; }
    kfree(j->body);
    kfree(j);
}

static void finish(struct netjob *j)
{
    u32 f = irq_save();
    j->done = true;
    bool orphan = j->released;
    irq_restore(f);
    if (orphan) free_job(j);
}

/* ---------------------------------------------------------------- commands */
static void do_ifconfig(struct netjob *j)
{
    char a[16], m[16], g[16], d[16];
    if (!g_net.present) { say(j, true, "no network card (QEMU: -nic user,model=e1000)"); return; }
    say(j, false, "eth0: Intel e1000  MAC %02x:%02x:%02x:%02x:%02x:%02x", g_net.mac[0], g_net.mac[1],
        g_net.mac[2], g_net.mac[3], g_net.mac[4], g_net.mac[5]);
    if (g_net.configured) {
        ip_to_str(g_net.ip, a); ip_to_str(g_net.mask, m); ip_to_str(g_net.gw, g); ip_to_str(g_net.dns, d);
        say(j, false, "      inet %s  netmask %s", a, m);
        say(j, false, "      gateway %s  dns %s", g, d);
    } else say(j, false, "      not configured (try 'dhcp')");
    say(j, false, "      RX %u packets (%u bytes)  TX %u packets (%u bytes)", g_net.rx_packets, g_net.rx_bytes,
        g_net.tx_packets, g_net.tx_bytes);
}

static void do_dhcp(struct netjob *j)
{
    say(j, false, "Requesting an address via DHCP...");
    int r = net_dhcp(6000);
    if (r) { say(j, true, "dhcp: %s", net_strerror(r)); return; }
    do_ifconfig(j);
}

static void do_ping(struct netjob *j)
{
    if (j->argc < 2) { say(j, true, "usage: ping <host> [count]"); return; }
    int count = j->argc > 2 ? atoi(j->args[2]) : 4;
    if (count < 1) count = 1;
    if (count > 100) count = 100;
    u32 ip;
    int r = net_resolve(j->args[1], &ip, 5000);
    if (r) { say(j, true, "ping: cannot resolve %s: %s", j->args[1], net_strerror(r)); return; }
    char a[16];
    ip_to_str(ip, a);
    say(j, false, "PING %s (%s): 32 data bytes", j->args[1], a);
    int ok = 0;
    u32 total = 0, best = 0xFFFFFFFF, worst = 0;
    for (int i = 0; i < count && !j->cancel; i++) {
        u32 rtt;
        u64 start = timer_ticks();
        r = net_ping(ip, (u16)(i + 1), 2000, &rtt);
        if (r == 0) {
            say(j, false, "Reply from %s: seq=%d time=%u ms", a, i + 1, rtt);
            ok++; total += rtt; best = MIN(best, rtt); worst = MAX(worst, rtt);
        } else say(j, true, "seq=%d: %s", i + 1, net_strerror(r));
        while (i + 1 < count && timer_ticks() - start < 1000 && !j->cancel) task_sleep(20);
    }
    say(j, false, "--- %s: %d sent, %d received, %d%% loss", a, count, ok, (count - ok) * 100 / count);
    if (ok) say(j, false, "rtt min/avg/max = %u/%u/%u ms", best, total / ok, worst);
}

static void do_nslookup(struct netjob *j)
{
    if (j->argc < 2) { say(j, true, "usage: nslookup <name> [server]"); return; }
    u32 ip, server = g_net.dns;
    if (j->argc > 2 && !ip_from_str(j->args[2], &server)) { say(j, true, "nslookup: bad server address"); return; }
    char d[16];
    ip_to_str(server, d);
    say(j, false, "Server: %s", d);
    int r = net_resolve_via(j->args[1], server, &ip, 6000);
    if (r) { say(j, true, "nslookup: %s: %s", j->args[1], net_strerror(r)); return; }
    char a[16];
    ip_to_str(ip, a);
    say(j, false, "Name:    %s", j->args[1]);
    say(j, false, "Address: %s", a);
}

static void do_http(struct netjob *j, bool save)
{
    if (j->argc < 2) { say(j, true, "usage: %s <http://host/path>%s", j->args[0], save ? " [file]" : ""); return; }
    char url[256];
    if (strncmp(j->args[1], "http://", 7)) snprintf(url, sizeof url, "http://%s", j->args[1]);
    else strlcpy(url, j->args[1], sizeof url);
    say(j, false, "Fetching %s ...", url);
    u64 t0 = timer_ticks();
    char *body;
    u32 len;
    int status;
    int r = http_get(url, &body, &len, &status, j->final_url, sizeof j->final_url);
    if (r) { say(j, true, "%s: %s", j->args[0], net_strerror(r)); return; }
    u32 ms = (u32)(timer_ticks() - t0);
    say(j, false, "HTTP %d, %u bytes in %u ms", status, len, ms);
    if (save) {
        const char *n = j->argc > 2 ? j->args[2] : strrchr(j->final_url + 7, '/');
        if (!n || !n[0] || !strcmp(n, "/")) n = "index.html";
        if (n[0] == '/') n++;
        if (!n[0]) n = "index.html";
        strlcpy(j->name, n, sizeof j->name);
        j->body = body;
        j->len = len;
        j->status = status;
    } else {
        /* print the body as text, at most 200 lines */
        int lines = 0;
        char *p = body;
        while (*p && lines < 200 && !j->cancel) {
            char *nl = strchr(p, '\n');
            int n = nl ? (int)(nl - p) : (int)strlen(p);
            char line[192];
            int k = 0;
            for (int i = 0; i < n && k < 190; i++) if (p[i] != '\r') line[k++] = p[i] == '\t' ? ' ' : p[i];
            line[k] = 0;
            say(j, false, "%s", line);
            lines++;
            p += n + (nl ? 1 : 0);
        }
        if (*p) say(j, false, "... (truncated)");
        kfree(body);
    }
}

static void job_main(void *arg)
{
    struct netjob *j = arg;
    if (j->is_fetch) {
        j->error = http_get(j->args[0], &j->body, &j->len, &j->status, j->final_url, sizeof j->final_url);
        finish(j);
        return;
    }
    const char *c = j->args[0];
    if (!g_net.present && strcmp(c, "ifconfig")) say(j, true, "%s: no network card", c);
    else if (!strcmp(c, "ifconfig")) do_ifconfig(j);
    else if (!strcmp(c, "dhcp")) do_dhcp(j);
    else if (!strcmp(c, "ping")) do_ping(j);
    else if (!strcmp(c, "nslookup")) do_nslookup(j);
    else if (!strcmp(c, "wget")) do_http(j, true);
    else if (!strcmp(c, "http")) do_http(j, false);
    finish(j);
}

/* ---------------------------------------------------------------- API */
bool netjob_is_command(const char *n)
{
    return !strcmp(n, "ifconfig") || !strcmp(n, "dhcp") || !strcmp(n, "ping") || !strcmp(n, "nslookup") ||
           !strcmp(n, "wget") || !strcmp(n, "http");
}

struct netjob *netjob_start(int argc, char **argv)
{
    struct netjob *j = kzalloc(sizeof *j);
    if (!j) return NULL;
    j->argc = MIN(argc, 8);
    for (int i = 0; i < j->argc; i++) strlcpy(j->args[i], argv[i], sizeof j->args[i]);
    task_create("netjob", job_main, j);
    return j;
}

struct netjob *netjob_fetch(const char *url)
{
    struct netjob *j = kzalloc(sizeof *j);
    if (!j) return NULL;
    j->is_fetch = true;
    strlcpy(j->args[0], url, sizeof j->args[0]);
    task_create("fetch", job_main, j);
    return j;
}

bool netjob_next_line(struct netjob *j, char *buf, int max, bool *is_error)
{
    u32 f = irq_save();
    if (!j->qn) { irq_restore(f); return false; }
    char *l = j->lines[j->qh];
    *is_error = j->err[j->qh];
    j->qh = (j->qh + 1) % QN;
    j->qn--;
    irq_restore(f);
    strlcpy(buf, l, max);
    kfree(l);
    return true;
}

bool netjob_done(struct netjob *j) { return j->done; }
void netjob_cancel(struct netjob *j) { j->cancel = true; }

bool netjob_take_download(struct netjob *j, char **data, u32 *len, char *name, int namemax)
{
    if (!j->done || !j->body) return false;
    *data = j->body;
    *len = j->len;
    strlcpy(name, j->name, namemax);
    j->body = NULL;
    return true;
}

bool netjob_fetch_result(struct netjob *j, char **body, u32 *len, int *status, char *final_url, int urlmax, int *err)
{
    if (!j->done) return false;
    *err = j->error;
    *body = j->body;
    *len = j->len;
    *status = j->status;
    strlcpy(final_url, j->final_url, urlmax);
    j->body = NULL;
    return true;
}

void netjob_release(struct netjob *j)
{
    if (!j) return;
    u32 f = irq_save();
    j->released = true;
    bool done = j->done;
    irq_restore(f);
    if (done) free_job(j);
}
