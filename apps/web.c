/*
 * NimbusOS - Web Browser (text mode, http:// only).
 *
 * Pages are fetched by a worker task (net/netjob.c) and turned into a list
 * of styled words by a forgiving HTML scanner: block tags become paragraph
 * breaks, h1-h6 headings, <a href> links, <li> bullets, <pre> keeps its
 * line breaks, <script>/<style> are skipped and common entities decoded.
 * The words are then laid out with the UI font to the window width.
 * There is no TLS, so https:// links are reported instead of followed.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "net/net.h"
#include "net/netjob.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define BAR_H 36
#define STATUS_H 22
#define MARGIN 14
#define LINE_H 18
#define MAXLINKS 256
#define HISTORY 16

enum { ST_NORMAL = 0, ST_BOLD = 1, ST_HEADING = 2, ST_PRE = 4 };
enum { IT_WORD, IT_BREAK, IT_PARA };

struct item { u8 type, style; i16 link; u32 off, len; };
struct run { int x, line; u32 off, len; u8 style; i16 link; };

struct web {
    textfield_t addr;
    char url[256], title[96], status[128];
    char history[HISTORY][256];
    int nhist;
    /* document */
    char *text; u32 tlen, tcap;
    struct item *items; int nitems, icap;
    char *links[MAXLINKS]; int nlinks;
    /* layout */
    struct run *runs; int nruns, rcap, nlines, laid_w;
    int scroll;                 /* in lines */
    struct netjob *job;
    bool addr_focus;
    rect_t b_back, b_go, content;
    int hover_link;
};

/* ---------------------------------------------------------------- document */
static void doc_clear(struct web *b)
{
    b->tlen = 0;
    b->nitems = 0;
    for (int i = 0; i < b->nlinks; i++) kfree(b->links[i]);
    b->nlinks = 0;
    b->nruns = 0;
    b->laid_w = 0;
    b->scroll = 0;
    b->title[0] = 0;
}

static void add_item(struct web *b, int type, int style, int link, const char *s, u32 len)
{
    if (type != IT_WORD && b->nitems) {
        struct item *last = &b->items[b->nitems - 1];
        if (last->type == IT_PARA) return;                         /* collapse breaks */
        if (last->type == IT_BREAK && type == IT_BREAK) { last->type = IT_PARA; return; }
        if (last->type == IT_BREAK && type == IT_PARA) { last->type = IT_PARA; return; }
    }
    if (type != IT_WORD && !b->nitems) return;
    if (b->nitems == b->icap) { b->icap = b->icap ? b->icap * 2 : 256; b->items = krealloc(b->items, b->icap * sizeof *b->items); }
    if (len && b->tlen + len + 1 > b->tcap) { b->tcap = (b->tlen + len + 1) * 2; b->text = krealloc(b->text, b->tcap); }
    struct item *it = &b->items[b->nitems++];
    it->type = (u8)type; it->style = (u8)style; it->link = (i16)link; it->off = b->tlen; it->len = len;
    if (len) { memcpy(b->text + b->tlen, s, len); b->tlen += len; }
}

static int entity(const char *p, int *consumed)
{
    static const struct { const char *n; int c; } ents[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' }, { "#39;", '\'' },
        { "nbsp;", ' ' }, { "copy;", 'c' }, { "mdash;", '-' }, { "ndash;", '-' }, { "hellip;", '.' },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(ents); i++) {
        int l = strlen(ents[i].n);
        if (!strncmp(p, ents[i].n, l)) { *consumed = l; return ents[i].c; }
    }
    if (p[0] == '#') {
        int k = 1, v = 0;
        bool hex = p[1] == 'x' || p[1] == 'X';
        if (hex) k++;
        while (isalnum(p[k]) && k < 8) { v = v * (hex ? 16 : 10) + (isdigit(p[k]) ? p[k] - '0' : (tolower(p[k]) - 'a' + 10)); k++; }
        if (p[k] == ';') { *consumed = k + 1; return v < 128 && v >= 32 ? v : '?'; }
    }
    *consumed = 0;
    return '&';
}

static void parse_html(struct web *b, const char *h, bool plain)
{
    int style = 0, link = -1, skip = 0;    /* skip > 0 while inside script/style/head junk */
    char word[256];
    int wl = 0;
#define FLUSH() do { if (wl) { add_item(b, IT_WORD, style, link, word, wl); wl = 0; } } while (0)
    if (plain) style = ST_PRE;
    for (const char *p = h; *p;) {
        if (*p == '<' && !plain) {
            FLUSH();
            if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p, "-->"); p = e ? e + 3 : p + strlen(p); continue; }
            const char *e = strchr(p, '>');
            if (!e) break;
            bool close = p[1] == '/';
            char tag[16];
            int tl = 0;
            const char *q = p + 1 + close;
            while (tl < 15 && (isalnum(*q))) tag[tl++] = (char)tolower(*q++);
            tag[tl] = 0;
            /* attributes: we only care about href */
            char href[256] = "";
            const char *hp = q;
            while (hp < e) {
                if (!strncasecmp(hp, "href", 4) && (hp[4] == '=' || hp[4] == ' ')) {
                    hp += 4;
                    while (*hp == ' ' || *hp == '=') hp++;
                    char quote = (*hp == '"' || *hp == '\'') ? *hp++ : 0;
                    int k = 0;
                    while (hp < e && *hp && (quote ? *hp != quote : *hp != ' ') && k < 255) href[k++] = *hp++;
                    href[k] = 0;
                    break;
                }
                hp++;
            }
            if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "noscript")) skip += close ? -1 : 1;
            if (skip < 0) skip = 0;
            if (!strcmp(tag, "title") && !close) {
                const char *te = strstr(e + 1, "</");
                if (te) {
                    int n = MIN((int)(te - e - 1), (int)sizeof b->title - 1);
                    memcpy(b->title, e + 1, n);
                    b->title[n] = 0;
                    p = te;
                    continue;
                }
            }
            if (!strcmp(tag, "br")) add_item(b, IT_BREAK, 0, -1, NULL, 0);
            else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "ul") || !strcmp(tag, "ol") ||
                     !strcmp(tag, "table") || !strcmp(tag, "tr") || !strcmp(tag, "section") || !strcmp(tag, "article") ||
                     !strcmp(tag, "header") || !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "blockquote") ||
                     !strcmp(tag, "form") || !strcmp(tag, "hr") || !strcmp(tag, "dl") || !strcmp(tag, "dt") || !strcmp(tag, "dd"))
                add_item(b, IT_PARA, 0, -1, NULL, 0);
            else if (!strcmp(tag, "li") && !close) { add_item(b, IT_BREAK, 0, -1, NULL, 0); add_item(b, IT_WORD, 0, -1, "\x07", 1); }
            else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
                add_item(b, IT_PARA, 0, -1, NULL, 0);
                if (close) style &= ~ST_HEADING; else style |= ST_HEADING;
            } else if (!strcmp(tag, "b") || !strcmp(tag, "strong")) { if (close) style &= ~ST_BOLD; else style |= ST_BOLD; }
            else if (!strcmp(tag, "pre")) { add_item(b, IT_PARA, 0, -1, NULL, 0); if (close) style &= ~ST_PRE; else style |= ST_PRE; }
            else if (!strcmp(tag, "a")) {
                if (!close && href[0] && b->nlinks < MAXLINKS) {
                    b->links[b->nlinks] = kmalloc(strlen(href) + 1);
                    strcpy(b->links[b->nlinks], href);
                    link = b->nlinks++;
                } else if (close) link = -1;
            }
            p = e + 1;
            continue;
        }
        if (skip) { p++; continue; }
        char c = *p;
        if (c == '&' && !plain) { int n; c = (char)entity(p + 1, &n); p += n; }
        if (c == '\r') { p++; continue; }
        if ((style & ST_PRE) && c == '\n') { FLUSH(); add_item(b, IT_BREAK, 0, -1, NULL, 0); p++; continue; }
        if (c == ' ' || c == '\t' || c == '\n') { FLUSH(); p++; continue; }
        if ((u8)c >= 32 && wl < 255) word[wl++] = (u8)c < 128 ? c : '?';
        p++;
    }
    FLUSH();
#undef FLUSH
}

/* ---------------------------------------------------------------- layout */
static int word_w(struct web *b, struct item *it)
{
    char tmp[256];
    memcpy(tmp, b->text + it->off, it->len);
    tmp[it->len] = 0;
    return (it->style & ST_PRE) ? (int)it->len * FONT_W : gfx_ui_width(tmp) + ((it->style & (ST_BOLD | ST_HEADING)) ? it->len : 0);
}

static void layout(window_t *w)
{
    struct web *b = w->app;
    int width = b->content.w - 2 * MARGIN - 16;
    if (width == b->laid_w) return;
    b->laid_w = width;
    b->nruns = 0;
    int x = 0, line = 0, space = gfx_ui_width(" ");
    for (int i = 0; i < b->nitems; i++) {
        struct item *it = &b->items[i];
        if (it->type == IT_BREAK) { x = 0; line++; continue; }
        if (it->type == IT_PARA) { x = 0; line += 2; continue; }
        int ww = word_w(b, it);
        if (x > 0 && x + space + ww > width) { x = 0; line++; }
        if (x > 0) x += space;
        if (b->nruns == b->rcap) { b->rcap = b->rcap ? b->rcap * 2 : 512; b->runs = krealloc(b->runs, b->rcap * sizeof *b->runs); }
        b->runs[b->nruns++] = (struct run){ x, line, it->off, it->len, it->style, it->link };
        x += ww;
    }
    b->nlines = line + 1;
}

static int vis_lines(struct web *b) { return MAX(1, (b->content.h - 2 * MARGIN) / LINE_H); }

/* ---------------------------------------------------------------- paint */
static void paint(window_t *w)
{
    struct web *b = w->app;
    surface_t *s = w->surface;
    b->content = rect_make(0, BAR_H, s->w, s->h - BAR_H - STATUS_H);
    layout(w);
    ui_toolbar(s, rect_make(0, 0, s->w, BAR_H));
    b->b_back = rect_make(6, 5, 30, 26);
    b->b_go = rect_make(s->w - 50, 5, 44, 26);
    ui_button(s, b->b_back, "<", b->nhist ? BTN_NORMAL : BTN_DISABLED);
    ui_button(s, b->b_go, b->job ? "..." : "Go", BTN_NORMAL);
    tf_draw(s, rect_make(42, 5, s->w - 98, 26), &b->addr, b->addr_focus);
    rect_t c = b->content;
    gfx_fill_rect(s, c.x, c.y, c.w, c.h, 0xFFFFFF);
    rect_t old = s->clip;
    gfx_set_clip(s, rect_make(c.x, c.y, c.w - 16, c.h));
    int first = b->scroll, last = b->scroll + vis_lines(b) + 1;
    for (int i = 0; i < b->nruns; i++) {
        struct run *r = &b->runs[i];
        if (r->line < first || r->line > last) continue;
        int x = c.x + MARGIN + r->x, y = c.y + MARGIN + (r->line - first) * LINE_H;
        char tmp[256];
        memcpy(tmp, b->text + r->off, r->len);
        tmp[r->len] = 0;
        u32 col = r->link >= 0 ? (r->link == b->hover_link ? RGB(200, 40, 40) : RGB(20, 70, 200)) : (r->style & ST_HEADING) ? RGB(20, 30, 60) : C_TEXT;
        int tw;
        if (r->style & ST_PRE) tw = gfx_text(s, x, y, tmp, col, COLOR_NONE);
        else if (r->style & (ST_BOLD | ST_HEADING)) tw = gfx_ui_text_bold(s, x, y, tmp, col);
        else tw = gfx_ui_text(s, x, y, tmp, col);
        if (r->link >= 0) gfx_hline(s, x, y + 15, tw, col);
    }
    s->clip = old;
    ui_vscroll(s, rect_make(c.x + c.w - 16, c.y, 16, c.h), b->nlines, vis_lines(b), b->scroll);
    int sy = s->h - STATUS_H;
    gfx_fill_rect(s, 0, sy, s->w, STATUS_H, RGB(240, 240, 240));
    gfx_hline(s, 0, sy, s->w, RGB(215, 215, 220));
    const char *st = b->status;
    if (b->hover_link >= 0 && b->hover_link < b->nlinks) st = b->links[b->hover_link];
    gfx_ui_text_ellipsis(s, 8, sy + 3, st, s->w - 16, C_TEXT);
    char t[128];
    snprintf(t, sizeof t, "%s - Web Browser", b->title[0] ? b->title : b->url);
    if (strcmp(t, w->title)) wm_set_title(w, t);
    w->mem_hint = b->tcap + b->icap * sizeof(struct item) + b->rcap * sizeof(struct run);
    wm_invalidate(w);
}

/* ---------------------------------------------------------------- navigation */
static const char *home_page =
    "<html><head><title>NimbusOS Web</title></head><body>"
    "<h1>Welcome to the NimbusOS Web Browser</h1>"
    "<p>This is a simple text-mode browser for <b>http://</b> pages. It has no TLS, so https sites "
    "cannot be opened. Type an address above and press Enter.</p>"
    "<h2>Try</h2><ul>"
    "<li><a href=\"http://10.0.2.2:8080/\">http://10.0.2.2:8080/</a> - a web server on your host computer "
    "(e.g. run <b>python3 -m http.server 8080</b>; QEMU's user network maps 10.0.2.2 to the host)</li>"
    "<li><a href=\"http://example.com/\">http://example.com/</a></li>"
    "<li><a href=\"http://neverssl.com/\">http://neverssl.com/</a></li>"
    "</ul><p>The Terminal has <b>ping</b>, <b>nslookup</b>, <b>wget</b>, <b>http</b> and <b>ifconfig</b> too.</p>"
    "</body></html>";

static void show_html(window_t *w, const char *html, bool plain)
{
    struct web *b = w->app;
    doc_clear(b);
    parse_html(b, html, plain);
    b->laid_w = 0;
    paint(w);
}

static void navigate(window_t *w, const char *url, bool push)
{
    struct web *b = w->app;
    char u[256];
    strlcpy(u, url, sizeof u);
    if (strncmp(u, "http://", 7) && strncmp(u, "about:", 6) && strncmp(u, "https://", 8)) snprintf(u, sizeof u, "http://%s", url);
    if (push && b->url[0] && strcmp(b->url, u)) {
        if (b->nhist == HISTORY) { memmove(b->history[0], b->history[1], (HISTORY - 1) * 256); b->nhist--; }
        strlcpy(b->history[b->nhist++], b->url, 256);
    }
    strlcpy(b->url, u, sizeof b->url);
    tf_set(&b->addr, u);
    b->addr.sel = b->addr.cursor;
    if (!strcmp(u, "about:home") || !strcmp(u, "about:blank")) {
        show_html(w, home_page, false);
        snprintf(b->status, sizeof b->status, "Built-in page");
        paint(w);
        return;
    }
    if (!strncmp(u, "https://", 8)) {
        doc_clear(b);
        show_html(w, "<h1>Secure sites are not supported</h1><p>NimbusOS has no TLS, so https:// pages cannot be "
                     "loaded. Try the http:// version of the site.</p>", false);
        snprintf(b->status, sizeof b->status, "https is not supported");
        paint(w);
        return;
    }
    if (b->job) { netjob_cancel(b->job); netjob_release(b->job); }
    b->job = netjob_fetch(u);
    snprintf(b->status, sizeof b->status, "Loading %s ...", u);
    klog("[web ] loading %s\n", u);
    wm_set_timer(w, 50);
    paint(w);
}

static void resolve_link(struct web *b, const char *href, char *out, int max)
{
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8) || !strncmp(href, "about:", 6)) { strlcpy(out, href, max); return; }
    if (!strncmp(href, "//", 2)) { snprintf(out, max, "http:%s", href); return; }
    /* base = scheme://host[:port] and the directory of the current path */
    char base[256];
    strlcpy(base, b->url, sizeof base);
    char *host_end = strchr(base + 7, '/');
    if (href[0] == '/') { if (host_end) *host_end = 0; snprintf(out, max, "%s%s", base, href); return; }
    if (!host_end) strlcat(base, "/", sizeof base);
    else { char *last = strrchr(base, '/'); last[1] = 0; }
    snprintf(out, max, "%s%s", base, href);
}

static int link_at(struct web *b, int x, int y)
{
    rect_t c = b->content;
    int line = b->scroll + (y - c.y - MARGIN) / LINE_H;
    for (int i = 0; i < b->nruns; i++) {
        struct run *r = &b->runs[i];
        if (r->line != line || r->link < 0) continue;
        char tmp[256];
        memcpy(tmp, b->text + r->off, r->len);
        tmp[r->len] = 0;
        int rx = c.x + MARGIN + r->x, rw = (r->style & ST_PRE) ? (int)r->len * FONT_W : gfx_ui_width(tmp);
        if (x >= rx && x < rx + rw + 4) return r->link;
    }
    return -1;
}

static void poll_job(window_t *w)
{
    struct web *b = w->app;
    if (!b->job || !netjob_done(b->job)) return;
    char *body = NULL;
    u32 len = 0;
    int status = 0, err = 0;
    char final_url[256];
    netjob_fetch_result(b->job, &body, &len, &status, final_url, sizeof final_url, &err);
    netjob_release(b->job);
    b->job = NULL;
    wm_set_timer(w, 0);
    if (err) {
        char page[512];
        snprintf(page, sizeof page, "<h1>Cannot open this page</h1><p>%s: %s.</p><p>Check the address and the network "
                 "(Terminal: <b>ifconfig</b>).</p>", b->url, net_strerror(err));
        show_html(w, page, false);
        snprintf(b->status, sizeof b->status, "Error: %s", net_strerror(err));
        klog("[web ] error %s\n", net_strerror(err));
    } else {
        strlcpy(b->url, final_url, sizeof b->url);
        tf_set(&b->addr, final_url);
        b->addr.sel = b->addr.cursor;
        bool html = strstr(body, "<html") || strstr(body, "<HTML") || strstr(body, "<p") || strstr(body, "<body") || strstr(body, "<h1");
        show_html(w, body, !html);
        snprintf(b->status, sizeof b->status, "Done - HTTP %d, %u bytes, %d links", status, len, b->nlinks);
        klog("[web ] loaded %s: HTTP %d, %u bytes, %d links, title \"%s\"\n", b->url, status, len, b->nlinks, b->title);
        kfree(body);
    }
    paint(w);
}

static void scroll_by(window_t *w, int d)
{
    struct web *b = w->app;
    b->scroll = MAX(0, MIN(MAX(0, b->nlines - vis_lines(b)), b->scroll + d));
    paint(w);
}

static void on_event(window_t *w, event_t *e)
{
    struct web *b = w->app;
    switch (e->type) {
    case EV_PAINT: b->laid_w = 0; paint(w); break;
    case EV_TIMER: poll_job(w); break;
    case EV_WHEEL: scroll_by(w, e->wheel * 3); break;
    case EV_MOUSE_MOVE: {
        int l = rect_contains(b->content, e->x, e->y) ? link_at(b, e->x, e->y) : -1;
        if (l != b->hover_link) { b->hover_link = l; paint(w); }
        break;
    }
    case EV_MOUSE_DOWN:
        b->addr_focus = rect_contains(rect_make(42, 5, w->surface->w - 98, 26), e->x, e->y);
        if (b->addr_focus) tf_click(&b->addr, rect_make(42, 5, w->surface->w - 98, 26), e->x);
        if (rect_contains(b->b_go, e->x, e->y)) { navigate(w, b->addr.text, true); break; }
        if (rect_contains(b->b_back, e->x, e->y) && b->nhist) { char u[256]; strlcpy(u, b->history[--b->nhist], sizeof u); navigate(w, u, false); break; }
        if (rect_contains(rect_make(b->content.x + b->content.w - 16, b->content.y, 16, b->content.h), e->x, e->y)) {
            b->scroll = ui_vscroll_offset_for(rect_make(0, b->content.y, 16, b->content.h), b->nlines, vis_lines(b), e->y);
            paint(w);
            break;
        }
        if (rect_contains(b->content, e->x, e->y)) {
            int l = link_at(b, e->x, e->y);
            if (l >= 0) {
                char u[256];
                resolve_link(b, b->links[l], u, sizeof u);
                klog("[web ] follow link %s\n", u);
                navigate(w, u, true);
                break;
            }
        }
        paint(w);
        break;
    case EV_KEY: {
        struct key_event *k = &e->key;
        if (!k->pressed) break;
        if (b->addr_focus) {
            if (k->key == KEY_ENTER) { b->addr_focus = false; navigate(w, b->addr.text, true); break; }
            if (k->key == KEY_ESC) { b->addr_focus = false; paint(w); break; }
            if (tf_key(&b->addr, k)) paint(w);
            break;
        }
        if (k->key == KEY_DOWN) scroll_by(w, 1);
        else if (k->key == KEY_UP) scroll_by(w, -1);
        else if (k->key == KEY_PGDN || k->key == ' ') scroll_by(w, vis_lines(b) - 1);
        else if (k->key == KEY_PGUP) scroll_by(w, -(vis_lines(b) - 1));
        else if (k->key == KEY_HOME) scroll_by(w, -1000000);
        else if (k->key == KEY_END) scroll_by(w, 1000000);
        else if (k->key == KEY_BACKSPACE && b->nhist) { char u[256]; strlcpy(u, b->history[--b->nhist], sizeof u); navigate(w, u, false); }
        else if ((k->mods & MOD_CTRL) && k->key == 'l') { b->addr_focus = true; b->addr.sel = 0; b->addr.cursor = b->addr.len; paint(w); }
        break;
    }
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY:
        if (b->job) { netjob_cancel(b->job); netjob_release(b->job); }
        doc_clear(b);
        kfree(b->text); kfree(b->items); kfree(b->runs); kfree(b);
        break;
    }
}

void web_open(const char *url)
{
    struct web *b = kzalloc(sizeof *b);
    if (!b) return;
    b->hover_link = -1;
    window_t *w = wm_create("Web Browser", ICON_WEB, 760, 520, WF_RESIZABLE, on_event, b);
    if (!w) { kfree(b); return; }
    w->min_w = 360;
    w->min_h = 200;
    wm_center(w);
    navigate(w, url && url[0] && url[0] != '/' ? url : "about:home", false);
}
