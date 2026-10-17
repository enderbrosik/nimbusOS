/*
 * NimbusOS - Web Browser (text mode, http:// and https://).
 *
 * Pages (http:// or https:// through the TLS 1.3 client in net/tls.c) are
 * fetched by a worker task (net/netjob.c) and turned into a list
 * of styled words by a forgiving HTML scanner: block tags become paragraph
 * breaks, h1-h6 headings, <a href> links, <li> bullets, <pre> keeps its
 * line breaks, <script>/<style> are skipped and common entities decoded.
 * The words are then laid out with the UI font to the window width.
 * https pages are encrypted, but the server's certificate is not checked.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "net/net.h"
#include "net/netjob.h"
#include "kernel/heap.h"
#include "kernel/settings.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define BAR_H 36
#define STATUS_H 22
#define MARGIN 14
#define LINE_H 18
#define MAXLINKS 256
#define HISTORY 16
/* Words typed in the address bar are searched with this (setting "search_url"). */
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="

enum { ST_NORMAL = 0, ST_BOLD = 1, ST_HEADING = 2, ST_PRE = 4, ST_UNDERLINE = 8, ST_COLOR = 16, ST_ITALIC = 32, ST_MARKER = 64, ST_RULE = 128 };
enum { IT_WORD, IT_BREAK, IT_PARA, IT_RULE, IT_GAP };
#define INDENT_PX 24                /* one level of list or blockquote indentation */

/* the computed style of an element (inherited by its children) */
struct cstyle { u8 bold, large, pre, underline, align, hidden, colored, italic, indent, marker; u32 color; };
struct css_rule { char sel[48]; u8 spec; u32 off, len; };     /* declarations are in web.css */

struct item { u8 type, style, align, glue, indent; i16 link; u32 off, len, color; };
struct run { int x, line, w; u32 off, len, color; u8 style, align; i16 link; };

struct web {
    textfield_t addr;
    char url[256], title[96], status[192];
    char history[HISTORY][256];
    int nhist;
    /* document */
    char *text; u32 tlen, tcap;
    struct item *items; int nitems, icap;
    char *links[MAXLINKS]; int nlinks;
    struct css_rule *rules; int nrules, rrcap;
    char *css; u32 csslen, csscap;          /* the declarations of the rules */
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
    b->nrules = 0;
    b->csslen = 0;
    b->nruns = 0;
    b->laid_w = 0;
    b->scroll = 0;
    b->title[0] = 0;
}

/* One item: a word, a line break or a paragraph break.  The style comes from
 * the element that produced it (NULL means no style). */
static void add_item(struct web *b, int type, const struct cstyle *c, int link, const char *s, u32 len, bool glue)
{
    static const struct cstyle none;
    if (!c) c = &none;
    u8 style = (c->bold ? ST_BOLD : 0) | (c->large ? ST_HEADING : 0) | (c->pre ? ST_PRE : 0) |
               (c->underline ? ST_UNDERLINE : 0) | (c->colored ? ST_COLOR : 0) | (c->italic ? ST_ITALIC : 0) |
               (c->marker ? ST_MARKER : 0) | (type == IT_RULE ? ST_RULE : 0);
    bool brk = type == IT_BREAK || type == IT_PARA;
    if (brk && b->nitems) {
        struct item *last = &b->items[b->nitems - 1];
        if (last->type == IT_PARA) return;                         /* collapse breaks */
        if (last->type == IT_BREAK && type == IT_BREAK) { last->type = IT_PARA; return; }
        if (last->type == IT_BREAK && type == IT_PARA) { last->type = IT_PARA; return; }
    }
    if ((brk || type == IT_GAP) && !b->nitems) return;
    if (b->nitems == b->icap) { b->icap = b->icap ? b->icap * 2 : 256; b->items = krealloc(b->items, b->icap * sizeof *b->items); }
    if (len && b->tlen + len + 1 > b->tcap) { b->tcap = (b->tlen + len + 1) * 2; b->text = krealloc(b->text, b->tcap); }
    struct item *it = &b->items[b->nitems++];
    it->type = (u8)type; it->style = style; it->align = c->align; it->glue = glue; it->indent = c->indent; it->link = (i16)link;
    it->color = c->color; it->off = b->tlen; it->len = len;
    if (len) { memcpy(b->text + b->tlen, s, len); b->tlen += len; }
}

/* The UI font is ASCII, so Unicode text is shown as its closest ASCII
 * spelling: curly quotes become straight ones, dashes '-', accented
 * letters their base letter, invisible characters nothing at all. */
static bool g_rich;       /* true: Latin-1 and Windows-1252 bytes for the UI font; false: plain ASCII (pre, titles) */

/* the Windows-1252 byte of a Unicode character (the font draws bytes 0x80-0x9F as these), or 0 */
static u8 cp1252_byte(u32 cp)
{
    static const struct { u16 cp; u8 b; } m[] = {
        { 0x20AC, 0x80 }, { 0x201A, 0x82 }, { 0x0192, 0x83 }, { 0x201E, 0x84 }, { 0x2026, 0x85 }, { 0x2020, 0x86 },
        { 0x2021, 0x87 }, { 0x2030, 0x89 }, { 0x0160, 0x8A }, { 0x2039, 0x8B }, { 0x0152, 0x8C }, { 0x017D, 0x8E },
        { 0x2018, 0x91 }, { 0x2019, 0x92 }, { 0x201C, 0x93 }, { 0x201D, 0x94 }, { 0x2022, 0x95 }, { 0x2013, 0x96 },
        { 0x2014, 0x97 }, { 0x2122, 0x99 }, { 0x0161, 0x9A }, { 0x203A, 0x9B }, { 0x0153, 0x9C }, { 0x017E, 0x9E },
        { 0x0178, 0x9F },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i].cp == cp) return m[i].b;
    return 0;
}

static const char *uni_ascii(u32 cp, char tmp[2])
{
    if (g_rich) {
        u8 b = cp >= 0xA1 && cp <= 0xFF && cp != 0xAD ? (u8)cp : cp1252_byte(cp);
        if (b) { tmp[0] = (char)b; tmp[1] = 0; return tmp; }
    }
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 32 && cp < 127) { tmp[0] = (char)cp; tmp[1] = 0; return tmp; }
    if (cp >= 0xC0 && cp <= 0xFF) {
        if (cp == 0xDF) return "ss";
        if (cp == 0xC6) return "AE";
        if (cp == 0xE6) return "ae";
        tmp[0] = latin1[cp - 0xC0]; tmp[1] = 0;
        return tmp;
    }
    switch (cp) {
    case 0x09: case 0x0A: case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF: return "";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0xB4: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0xB7: case 0x2027: case 0x25CF: return "*";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0x2122: return "(TM)";
    case 0xAB: return "<<";
    case 0xBB: return ">>";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0xD7: return "x";
    case 0xF7: return "/";
    case 0xB0: return " deg";
    case 0xB1: return "+/-";
    case 0xBD: return "1/2";
    case 0xBC: return "1/4";
    case 0xBE: return "3/4";
    case 0x20AC: return "EUR";
    case 0xA3: return "GBP";
    case 0xA5: return "JPY";
    case 0xA2: return "c";
    case 0xA7: return "S";
    case 0xB6: return "P";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x2194: return "<->";
    case 0x21D2: return "=>";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2260: return "!=";
    case 0x2248: return "~";
    case 0x221E: return "inf";
    case 0x2713: case 0x2714: return "v";
    case 0x00BF: return "?";
    case 0x00A1: return "!";
    default: return "?";
    }
}

/* Decode one character at p (UTF-8, or a Latin-1 byte when the bytes are
 * not valid UTF-8); returns its code point and sets *len. */
static u32 utf8_decode(const char *p, int *len)
{
    const u8 *u = (const u8 *)p;
    int n = u[0] >= 0xF0 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC2 ? 2 : 1;
    if (n > 1 && u[0] < 0xF5) {
        u32 cp = u[0] & (0x7F >> n);
        int k = 1;
        for (; k < n && (u[k] & 0xC0) == 0x80; k++) cp = cp << 6 | (u[k] & 0x3F);
        if (k == n) { *len = n; return cp; }
    }
    *len = 1;
    return u[0];
}

/* "&name;" or "&#123;"; returns the text it stands for and sets *consumed
 * (0 if this is not an entity, then the '&' is literal). */
static const char *entity(const char *p, int *consumed, char tmp[2])
{
    static const struct { const char *n; u32 cp; } ents[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", 0xA0 },
        { "copy;", 0xA9 }, { "reg;", 0xAE }, { "trade;", 0x2122 }, { "mdash;", 0x2014 }, { "ndash;", 0x2013 },
        { "hellip;", 0x2026 }, { "lsquo;", 0x2018 }, { "rsquo;", 0x2019 }, { "ldquo;", 0x201C }, { "rdquo;", 0x201D },
        { "laquo;", 0xAB }, { "raquo;", 0xBB }, { "bull;", 0x2022 }, { "middot;", 0xB7 }, { "times;", 0xD7 },
        { "divide;", 0xF7 }, { "deg;", 0xB0 }, { "plusmn;", 0xB1 }, { "euro;", 0x20AC }, { "pound;", 0xA3 },
        { "yen;", 0xA5 }, { "cent;", 0xA2 }, { "sect;", 0xA7 }, { "para;", 0xB6 }, { "larr;", 0x2190 },
        { "rarr;", 0x2192 }, { "uarr;", 0x2191 }, { "darr;", 0x2193 }, { "le;", 0x2264 }, { "ge;", 0x2265 },
        { "ne;", 0x2260 }, { "szlig;", 0xDF }, { "shy;", 0xAD }, { "thinsp;", 0x2009 }, { "zwj;", 0x200D },
        { "zwnj;", 0x200C }, { "frac12;", 0xBD },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(ents); i++) {
        int l = strlen(ents[i].n);
        if (!strncmp(p, ents[i].n, l)) { *consumed = l; return uni_ascii(ents[i].cp, tmp); }
    }
    if (p[0] == '#') {
        int k = 1;
        u32 v = 0;
        bool hex = p[1] == 'x' || p[1] == 'X';
        if (hex) k++;
        while (k < 10 && (hex ? isalnum(p[k]) : isdigit(p[k]))) {
            v = v * (hex ? 16 : 10) + (isdigit(p[k]) ? p[k] - '0' : (tolower(p[k]) - 'a' + 10));
            k++;
        }
        if (p[k] == ';' && k > (hex ? 2 : 1)) { *consumed = k + 1; return uni_ascii(v, tmp); }
    }
    /* accented letters: &eacute; &Uuml; &ccedil; &atilde; &aring; ... -> the letter */
    if (isalpha(p[0])) {
        /* letters that have an accented form, and the Latin-1 codes of those forms */
        static const struct { const char *mark, *letters, *codes; } acc[] = {
            { "acute;", "AEIOUYaeiouy", "\xC1\xC9\xCD\xD3\xDA\xDD\xE1\xE9\xED\xF3\xFA\xFD" },
            { "grave;", "AEIOUaeiou", "\xC0\xC8\xCC\xD2\xD9\xE0\xE8\xEC\xF2\xF9" },
            { "uml;", "AEIOUaeiouy", "\xC4\xCB\xCF\xD6\xDC\xE4\xEB\xEF\xF6\xFC\xFF" },
            { "circ;", "AEIOUaeiou", "\xC2\xCA\xCE\xD4\xDB\xE2\xEA\xEE\xF4\xFB" },
            { "tilde;", "ANOano", "\xC3\xD1\xD5\xE3\xF1\xF5" },
            { "cedil;", "Cc", "\xC7\xE7" },
            { "ring;", "Aa", "\xC5\xE5" },
            { "slash;", "Oo", "\xD8\xF8" },
            { "caron;", "", "" },
        };
        for (unsigned i = 0; i < ARRAY_SIZE(acc); i++) {
            int l = strlen(acc[i].mark);
            if (strncmp(p + 1, acc[i].mark, l)) continue;
            *consumed = 1 + l;
            for (int k = 0; acc[i].letters[k]; k++)
                if (acc[i].letters[k] == p[0]) return uni_ascii((u8)acc[i].codes[k], tmp);
            tmp[0] = p[0]; tmp[1] = 0;                  /* no such accented letter: the plain one */
            return tmp;
        }
    }
    *consumed = 0;
    return "&";
}

/* ---------------------------------------------------------------- CSS
 * Core CSS: <style> blocks (collected before the page is read, so rules
 * apply wherever they appear) and style="" attributes.  A selector is one
 * compound (tag, .class, #id, or a combination); in a descendant selector
 * only the last compound counts.  Pseudo-classes and at-rules (@media ...)
 * are skipped.  Properties: color, background-color (ignored), font-weight,
 * font-size, font-style (italic is slanted text), text-align,
 * text-decoration (underline), display (none).  Rules apply by specificity
 * (tag, then class, then id), then the style attribute. */

static bool ci_equal(const char *a, int n, const char *lit)
{
    return n == (int)strlen(lit) && !strncasecmp(a, lit, n);
}

static bool has_word(const char *v, int n, const char *w)
{
    int wl = strlen(w);
    for (int i = 0; i + wl <= n; i++)
        if (!strncasecmp(v + i, w, wl) && (i == 0 || v[i - 1] == ' ') && (i + wl == n || v[i + wl] == ' ')) return true;
    return false;
}

static const char *skip_space(const char *p, const char *e)
{
    while (p < e && isspace((u8)*p)) p++;
    return p;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static const struct { const char *name; u32 rgb; } named_colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "lime", 0x00FF00 }, { "blue", 0x0000FF }, { "navy", 0x000080 }, { "darkblue", 0x00008B },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "lightgray", 0xD3D3D3 },
    { "darkgray", 0xA9A9A9 }, { "maroon", 0x800000 }, { "darkred", 0x8B0000 }, { "purple", 0x800080 },
    { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF },
    { "magenta", 0xFF00FF }, { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "gold", 0xFFD700 },
    { "olive", 0x808000 }, { "brown", 0xA52A2A }, { "pink", 0xFFC0CB }, { "darkgreen", 0x006400 },
};

/* #rgb, #rrggbb, rgb(r, g, b) or a basic colour name */
static bool parse_color(const char *v, int n, u32 *out)
{
    if (n > 0 && v[0] == '#') {
        u32 h = 0;
        for (int i = 1; i < n; i++) {
            int d = hex_digit(v[i]);
            if (d < 0) return false;
            h = h * 16 + d;
        }
        if (n == 4) { u32 r = (h >> 8) & 15, g = (h >> 4) & 15, b = h & 15; *out = RGB(r * 17, g * 17, b * 17); return true; }
        if (n == 7) { *out = h; return true; }
        return false;
    }
    if (n > 4 && !strncasecmp(v, "rgb(", 4)) {
        u32 part[3] = { 0, 0, 0 };
        int k = 0;
        for (int i = 4; i < n && k < 3; i++) {
            if (v[i] >= '0' && v[i] <= '9') part[k] = part[k] * 10 + (v[i] - '0');
            else if (v[i] == ',' || v[i] == ')') k++;
        }
        for (int i = 0; i < 3; i++) if (part[i] > 255) part[i] = 255;
        *out = RGB(part[0], part[1], part[2]);
        return true;
    }
    for (unsigned i = 0; i < sizeof named_colors / sizeof named_colors[0]; i++)
        if (ci_equal(v, n, named_colors[i].name)) { *out = named_colors[i].rgb; return true; }
    return false;
}

/* a length such as 12px, 1.5em, 120%, 10pt: returns it in tenths of its unit */
static int css_length(const char *v, int n, char unit[4])
{
    int i = 0, val = 0, dec = 0;
    bool any = false;
    while (i < n && isdigit((u8)v[i])) { val = val * 10 + (v[i++] - '0'); any = true; }
    if (i < n && v[i] == '.') {
        i++;
        if (i < n && isdigit((u8)v[i])) dec = v[i++] - '0';
    }
    if (!any) return -1;
    int j = 0;
    while (i < n && j < 3 && (isalpha((u8)v[i]) || v[i] == '%')) unit[j++] = (char)tolower((u8)v[i++]);
    unit[j] = 0;
    return val * 10 + dec;
}

static void css_font_size(struct cstyle *c, const char *v, int n)
{
    if (ci_equal(v, n, "large") || ci_equal(v, n, "x-large") || ci_equal(v, n, "xx-large") ||
        ci_equal(v, n, "xxx-large") || ci_equal(v, n, "larger")) { c->large = 1; return; }
    if (ci_equal(v, n, "medium") || ci_equal(v, n, "small") || ci_equal(v, n, "x-small") ||
        ci_equal(v, n, "xx-small") || ci_equal(v, n, "smaller") || ci_equal(v, n, "normal")) { c->large = 0; return; }
    char u[4];
    int t = css_length(v, n, u);
    if (t < 0) return;
    int px10;                                 /* the size in tenths of a pixel; body text is 14 px */
    if (!u[0] || !strcmp(u, "px")) px10 = t;
    else if (!strcmp(u, "pt")) px10 = t * 4 / 3;
    else if (!strcmp(u, "em") || !strcmp(u, "rem")) px10 = t * 14;
    else if (!strcmp(u, "%")) px10 = t * 14 / 100;
    else return;
    c->large = px10 >= 180;                   /* 18 px and up uses the heading face */
}

static void apply_decl(struct cstyle *c, const char *name, int nl, const char *v, int vl)
{
    char u[4];
    if (ci_equal(name, nl, "color")) {
        u32 col;
        if (parse_color(v, vl, &col)) { c->color = col; c->colored = 1; }
    } else if (ci_equal(name, nl, "font-weight")) {
        if (ci_equal(v, vl, "bold") || ci_equal(v, vl, "bolder")) c->bold = 1;
        else if (ci_equal(v, vl, "normal") || ci_equal(v, vl, "lighter")) c->bold = 0;
        else { int t = css_length(v, vl, u); if (t >= 0 && !u[0] && t / 10 >= 600) c->bold = 1; else if (t >= 0 && !u[0]) c->bold = 0; }
    } else if (ci_equal(name, nl, "font-style")) {
        if (ci_equal(v, vl, "italic") || ci_equal(v, vl, "oblique")) c->italic = 1;
        else if (ci_equal(v, vl, "normal")) c->italic = 0;
    } else if (ci_equal(name, nl, "font-size")) {
        css_font_size(c, v, vl);
    } else if (ci_equal(name, nl, "text-align")) {
        if (ci_equal(v, vl, "center")) c->align = 1;
        else if (ci_equal(v, vl, "right") || ci_equal(v, vl, "end")) c->align = 2;
        else if (ci_equal(v, vl, "left") || ci_equal(v, vl, "start") || ci_equal(v, vl, "justify")) c->align = 0;
    } else if (ci_equal(name, nl, "text-decoration") || ci_equal(name, nl, "text-decoration-line")) {
        if (has_word(v, vl, "underline")) c->underline = 1;
        else if (has_word(v, vl, "none")) c->underline = 0;
    } else if (ci_equal(name, nl, "display")) {
        c->hidden = ci_equal(v, vl, "none");
    }
}

/* a declaration list such as "color: red; font-weight: bold" (not NUL terminated) */
static void apply_decls(struct cstyle *c, const char *d, int n)
{
    const char *end = d + n;
    while (d < end) {
        const char *semi = d;
        while (semi < end && *semi != ';') semi++;
        const char *colon = d;
        while (colon < semi && *colon != ':') colon++;
        if (colon < semi) {
            const char *ns = skip_space(d, colon), *ne = colon;
            while (ne > ns && isspace((u8)ne[-1])) ne--;
            const char *vs = skip_space(colon + 1, semi), *ve = semi;
            for (const char *b = vs; b < semi; b++) if (*b == '!') { ve = b; break; }   /* drop !important */
            while (ve > vs && isspace((u8)ve[-1])) ve--;
            apply_decl(c, ns, ne - ns, vs, ve - vs);
        }
        d = semi + 1;
    }
}

static bool word_in(const char *list, const char *w, int wl)
{
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e - p == wl && !strncmp(p, w, wl)) return true;
        p = e;
    }
    return false;
}

/* does a single compound selector ("p", ".note", "#main", "div.box") match? */
static bool compound_match(const char *sel, const char *tag, const char *idv, const char *cls)
{
    char buf[64];
    strlcpy(buf, sel, sizeof buf);
    char *p = buf;
    if (*p == '*') p++;
    else if (*p && *p != '.' && *p != '#') {
        char *e = p;
        while (*e && *e != '.' && *e != '#') e++;
        char save = *e;
        *e = 0;
        bool same = !strcasecmp(p, tag);
        *e = save;
        if (!same) return false;
        p = e;
    }
    while (*p) {
        char kind = *p++;
        char *e = p;
        while (*e && *e != '.' && *e != '#') e++;
        int wl = e - p;
        if (kind == '.') { if (!word_in(cls, p, wl)) return false; }
        else if (kind == '#') { if (!idv || (int)strlen(idv) != wl || strncmp(idv, p, wl)) return false; }
        else return false;
        p = e;
    }
    return true;
}

static void add_rule(struct web *b, const char *sel, int sl, const char *decl, int dl)
{
    if (sl <= 0 || sl >= 48 || dl <= 0) return;
    /* the last compound of a descendant selector, without pseudo-classes or attributes */
    const char *last = sel + sl;
    while (last > sel && !isspace((u8)last[-1]) && last[-1] != '>' && last[-1] != '+' && last[-1] != '~') last--;
    int n = sel + sl - last;
    if (n <= 0 || n >= 48) return;
    for (int i = 0; i < n; i++) if (last[i] == ':' || last[i] == '[' || last[i] == '(') return;
    if (b->nrules == b->rrcap) {
        b->rrcap = b->rrcap ? b->rrcap * 2 : 64;
        b->rules = krealloc(b->rules, b->rrcap * sizeof *b->rules);
    }
    if (b->csslen + dl + 1 > b->csscap) {
        b->csscap = (b->csslen + dl + 1) * 2;
        b->css = krealloc(b->css, b->csscap);
    }
    struct css_rule *r = &b->rules[b->nrules++];
    memcpy(r->sel, last, n);
    r->sel[n] = 0;
    bool has_id = false, has_class = false;
    for (int i = 0; i < n; i++) { has_id |= last[i] == '#'; has_class |= last[i] == '.'; }
    r->spec = has_id ? 3 : has_class ? 2 : 1;
    r->off = b->csslen;
    r->len = dl;
    memcpy(b->css + b->csslen, decl, dl);
    b->csslen += dl;
    b->css[b->csslen] = 0;
}

/* Reads a stylesheet: rules are "selectors { declarations }", comments are skipped. */
static void parse_css(struct web *b, const char *c, const char *end)
{
    while (c < end) {
        c = skip_space(c, end);
        if (c + 1 < end && c[0] == '/' && c[1] == '*') {
            const char *e = c + 2;
            while (e + 1 < end && !(e[0] == '*' && e[1] == '/')) e++;
            c = e + 2 < end ? e + 2 : end;
            continue;
        }
        if (c >= end) break;
        if (*c == '@') {                      /* at-rule: skip a ;-terminated one or a whole block */
            const char *s = c, *semi = c;
            while (semi < end && *semi != ';' && *semi != '{') semi++;
            if (semi < end && *semi == ';') { c = semi + 1; continue; }
            s = semi;
            int depth = 0;
            for (; s < end; s++) {
                if (*s == '{') depth++;
                else if (*s == '}' && --depth == 0) break;
            }
            c = s + 1;
            continue;
        }
        const char *open = c;
        while (open < end && *open != '{') open++;
        if (open >= end) break;
        const char *close = open + 1;
        int depth = 1;
        while (close < end && depth) {
            if (*close == '{') depth++;
            else if (*close == '}') depth--;
            if (depth) close++;
        }
        for (const char *s = c; s < open;) {          /* selector list: "a, b.c, #d" */
            const char *e = s;
            while (e < open && *e != ',') e++;
            const char *ss = skip_space(s, e), *se = e;
            while (se > ss && isspace((u8)se[-1])) se--;
            add_rule(b, ss, se - ss, open + 1, close - open - 1);
            s = e + 1;
        }
        c = close + 1;
    }
}

/* the case-insensitive position of needle in h, or NULL */
static const char *find_ci(const char *h, const char *needle)
{
    int nl = strlen(needle);
    for (; *h; h++)
        if (!strncasecmp(h, needle, nl)) return h;
    return NULL;
}

/* First pass: every <style> block in the page, so that rules apply everywhere. */
static void collect_css(struct web *b, const char *h)
{
    for (const char *p = find_ci(h, "<style"); p; p = find_ci(p + 6, "<style")) {
        const char *open = strchr(p, '>');
        if (!open) break;
        const char *close = find_ci(open + 1, "</style");
        if (!close) close = open + strlen(open);
        parse_css(b, open + 1, close);
        p = close;
    }
}

/* ---------------------------------------------------------------- parsing */
/* Decodes the entities in an attribute value in place: &amp; &lt; &gt; &quot; &apos; &#39; &#x27; */
static void unescape_attr(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '&') {
            if (!strncmp(s, "&amp;", 5)) { *o++ = '&'; s += 5; continue; }
            if (!strncmp(s, "&lt;", 4)) { *o++ = '<'; s += 4; continue; }
            if (!strncmp(s, "&gt;", 4)) { *o++ = '>'; s += 4; continue; }
            if (!strncmp(s, "&quot;", 6)) { *o++ = '"'; s += 6; continue; }
            if (!strncmp(s, "&apos;", 6)) { *o++ = '\''; s += 6; continue; }
            if (s[1] == '#') {
                const char *d = s + 2;
                int base = 10, v = 0, n = 0;
                if (*d == 'x' || *d == 'X') { base = 16; d++; }
                while (n < 6 && hex_digit(*d) >= 0 && (base == 16 || isdigit((u8)*d))) { v = v * base + hex_digit(*d); d++; n++; }
                if (n && *d == ';' && v >= 32 && v < 127) { *o++ = (char)v; s = (char *)d + 1; continue; }
            }
        }
        *o++ = *s++;
    }
    *o = 0;
}

/* Attributes of a tag (between q and e): href, class, id and style are kept. */
static void tag_attrs(const char *q, const char *e, char *href, char *cls, char *idv, char *inl)
{
    const char *p = q;
    while (p < e) {
        while (p < e && (isspace((u8)*p) || *p == '/')) p++;
        const char *ns = p;
        while (p < e && (isalnum((u8)*p) || *p == '-' || *p == '_')) p++;
        int nl = p - ns;
        p = skip_space(p, e);
        char val[256];
        int vl = 0;
        if (p < e && *p == '=') {
            p = skip_space(p + 1, e);
            char quote = (p < e && (*p == '"' || *p == '\'')) ? *p++ : 0;
            while (p < e && vl < 255 && (quote ? *p != quote : !isspace((u8)*p))) val[vl++] = *p++;
            if (quote && p < e && *p == quote) p++;
        }
        val[vl] = 0;
        if (nl == 0) { p++; continue; }
        char *dst = ci_equal(ns, nl, "href") ? href : ci_equal(ns, nl, "class") ? cls
                  : ci_equal(ns, nl, "id") ? idv : ci_equal(ns, nl, "style") ? inl : NULL;
        if (dst) {
            int cap = dst == cls ? 128 : dst == idv ? 64 : 256;
            int n = vl < cap - 1 ? vl : cap - 1;
            memcpy(dst, val, n);
            dst[n] = 0;
            if (dst == href) unescape_attr(dst);
        }
    }
}

/* The style of an element: its parent's style, then the tag's defaults,
 * then the matching stylesheet rules, then its style attribute. */
static void element_style(struct web *b, struct cstyle *c, const char *tag, const char *idv, const char *cls, const char *inl)
{
    if (!strcmp(tag, "b") || !strcmp(tag, "strong")) c->bold = 1;
    else if (!strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3")) { c->bold = 1; c->large = 1; c->colored = 1; c->color = RGB(20, 30, 60); }
    else if (tag[0] == 'h' && tag[1] >= '4' && tag[1] <= '6' && !tag[2]) { c->bold = 1; c->colored = 1; c->color = RGB(20, 30, 60); }
    else if (!strcmp(tag, "pre")) c->pre = 1;
    else if (!strcmp(tag, "u") || !strcmp(tag, "ins")) c->underline = 1;
    else if (!strcmp(tag, "center")) c->align = 1;
    else if (!strcmp(tag, "i") || !strcmp(tag, "em") || !strcmp(tag, "cite") || !strcmp(tag, "var") ||
             !strcmp(tag, "dfn") || !strcmp(tag, "address")) c->italic = 1;
    else if (!strcmp(tag, "ul") || !strcmp(tag, "ol") || !strcmp(tag, "menu") || !strcmp(tag, "dir") ||
             !strcmp(tag, "blockquote") || !strcmp(tag, "dd")) { if (c->indent < 6) c->indent++; }
    else if (!strcmp(tag, "a")) c->colored = 0;      /* links keep their own colour unless a rule sets one */
    for (int spec = 1; spec <= 3; spec++)
        for (int i = 0; i < b->nrules; i++) {
            struct css_rule *r = &b->rules[i];
            if (r->spec == spec && compound_match(r->sel, tag, idv, cls)) apply_decls(c, b->css + r->off, r->len);
        }
    if (inl[0]) apply_decls(c, inl, strlen(inl));
}

static bool block_tag(const char *t)
{
    static const char *names[] = { "p", "div", "ul", "ol", "li", "table", "tr", "td", "th", "section", "article", "header",
        "footer", "nav", "blockquote", "form", "dl", "dt", "dd", "pre", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6" };
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) if (!strcmp(t, names[i])) return true;
    return false;
}

static bool void_tag(const char *t)
{
    return !strcmp(t, "br") || !strcmp(t, "hr") || !strcmp(t, "img") || !strcmp(t, "meta") ||
           !strcmp(t, "link") || !strcmp(t, "input") || !strcmp(t, "wbr") || !strcmp(t, "col");
}

#define MAXDEPTH 48

static void parse_html(struct web *b, const char *h, bool plain)
{
    struct cstyle stack[MAXDEPTH];
    char stag[MAXDEPTH][16];
    int depth = 0;
    memset(stack, 0, sizeof stack);
    struct cstyle *cur = &stack[0];
    if (plain) cur->pre = 1;
    if (!plain) collect_css(b, h);
    int link = -1, skip = 0;               /* skip > 0 while inside script/noscript */
    int olcount[MAXDEPTH];                 /* the last number used by each open <ol> */
    memset(olcount, 0, sizeof olcount);
    char word[256];
    int wl = 0;
    bool word_glue = false;                /* the word directly follows text or a tag */
    char prev = 0;                         /* the previous source character */
#define FLUSH() do { if (wl && !cur->hidden) add_item(b, IT_WORD, cur, link, word, wl, word_glue); wl = 0; } while (0)
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
            while (tl < 15 && isalnum((u8)*q)) tag[tl++] = (char)tolower((u8)*q++);
            tag[tl] = 0;
            if (block_tag(tag)) prev = ' ';              /* what follows starts a new block: it never glues */
            char href[256] = "", cls[128] = "", idv[64] = "", inl[256] = "";
            if (!close) tag_attrs(q, e, href, cls, idv, inl);
            if (!strcmp(tag, "script") || !strcmp(tag, "noscript")) skip += close ? -1 : 1;
            if (skip < 0) skip = 0;
            if (!strcmp(tag, "style") && !close) {          /* collected by collect_css */
                const char *te = find_ci(e + 1, "</style");
                p = te ? (strchr(te, '>') ? strchr(te, '>') + 1 : te + strlen(te)) : e + strlen(e);
                continue;
            }
            if (!strcmp(tag, "title") && !close) {
                const char *te = strstr(e + 1, "</");
                if (te) {
                    /* convert entities / UTF-8 the same way as body text */
                    g_rich = false;
                    int o = 0;
                    for (const char *t = e + 1; t < te && o < (int)sizeof b->title - 4;) {
                        char tmp[2];
                        const char *text;
                        int n = 1;
                        if (*t == '&') { text = entity(t + 1, &n, tmp); n++; }
                        else if ((u8)*t >= 0x80) text = uni_ascii(utf8_decode(t, &n), tmp);
                        else if ((u8)*t < 32) text = " ";
                        else { tmp[0] = *t; tmp[1] = 0; text = tmp; }
                        for (; *text && o < (int)sizeof b->title - 4; text++) b->title[o++] = *text;
                        t += n;
                    }
                    b->title[o] = 0;
                    p = te;
                    continue;
                }
            }
            if (close) {
                int i = depth - 1;
                while (i >= 0 && strcmp(stag[i], tag)) i--;
                if (i >= 0) {                                 /* close everything opened inside it too */
                    depth = i;
                    cur = depth ? &stack[depth - 1] : &stack[0];
                    if (!strcmp(tag, "pre") || (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') || !strcmp(tag, "p") ||
                        !strcmp(tag, "div") || !strcmp(tag, "blockquote") || !strcmp(tag, "table") || !strcmp(tag, "dl"))
                        add_item(b, IT_PARA, cur, -1, NULL, 0, false);
                    else if (!strcmp(tag, "ul") || !strcmp(tag, "ol"))      /* a gap after a list; a nested list just ends the line */
                        add_item(b, depth > 0 && !strcmp(stag[depth - 1], "li") ? IT_BREAK : IT_PARA, cur, -1, NULL, 0, false);
                    if (!strcmp(tag, "a")) link = -1;
                    if ((!strcmp(tag, "td") || !strcmp(tag, "th")) && !cur->hidden) add_item(b, IT_GAP, cur, -1, NULL, 0, false);
                }
                if (!strcmp(tag, "a") && i < 0) link = -1;
            } else {
                bool hidden = cur->hidden;
                if (!hidden) {
                    if (!strcmp(tag, "br") || !strcmp(tag, "tr")) add_item(b, IT_BREAK, cur, -1, NULL, 0, false);
                    else if (!strcmp(tag, "hr")) {
                        add_item(b, IT_PARA, cur, -1, NULL, 0, false);
                        add_item(b, IT_RULE, cur, -1, NULL, 0, false);
                        add_item(b, IT_PARA, cur, -1, NULL, 0, false);
                    } else if ((!strcmp(tag, "ul") || !strcmp(tag, "ol")) && depth > 0 && !strcmp(stag[depth - 1], "li")) {
                        add_item(b, IT_BREAK, cur, -1, NULL, 0, false);       /* a list inside an item */
                    } else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "ul") || !strcmp(tag, "ol") ||
                             !strcmp(tag, "table") || !strcmp(tag, "section") || !strcmp(tag, "article") ||
                             !strcmp(tag, "header") || !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "blockquote") ||
                             !strcmp(tag, "form") || !strcmp(tag, "dl") || !strcmp(tag, "dt") || !strcmp(tag, "dd") ||
                             !strcmp(tag, "pre") || (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]))
                        add_item(b, IT_PARA, cur, -1, NULL, 0, false);
                    if (!strcmp(tag, "li")) {              /* a bullet, or the next number of the nearest <ol> */
                        char mk[12] = "\x95";
                        int k = depth - 1;
                        while (k >= 0 && strcmp(stag[k], "ul") && strcmp(stag[k], "ol") && strcmp(stag[k], "menu") && strcmp(stag[k], "dir")) k--;
                        if (k >= 0 && !strcmp(stag[k], "ol")) snprintf(mk, sizeof mk, "%d.", ++olcount[k]);
                        struct cstyle m = *cur;
                        m.marker = 1;
                        bool at_line_start = b->nitems && (b->items[b->nitems - 1].type == IT_BREAK || b->items[b->nitems - 1].type == IT_PARA);
                        if (!at_line_start) add_item(b, IT_BREAK, cur, -1, NULL, 0, false);
                        add_item(b, IT_WORD, &m, -1, mk, strlen(mk), false);
                    }
                }
                if (!void_tag(tag) && depth < MAXDEPTH) {
                    stack[depth] = *cur;
                    element_style(b, &stack[depth], tag, idv, cls, inl);
                    if (!hidden && stack[depth].hidden) stack[depth].hidden = 1;
                    if (hidden) stack[depth].hidden = 1;                /* hidden inherits */
                    strlcpy(stag[depth], tag, sizeof stag[depth]);
                    olcount[depth] = 0;
                    depth++;
                    cur = &stack[depth - 1];
                    if (!strcmp(tag, "a") && href[0] && b->nlinks < MAXLINKS) {
                        b->links[b->nlinks] = kmalloc(strlen(href) + 1);
                        strcpy(b->links[b->nlinks], href);
                        link = b->nlinks++;
                    }
                }
            }
            p = e + 1;
            continue;
        }
        if (skip || cur->hidden) { p++; continue; }
        char c = *p;
        g_rich = !cur->pre && !plain;
        if (c == '\r') { p++; continue; }
        if (cur->pre && c == '\n') { FLUSH(); add_item(b, IT_BREAK, cur, -1, NULL, 0, false); prev = ' '; p++; continue; }
        if (c == ' ' || c == '\t' || c == '\n') { FLUSH(); prev = ' '; p++; continue; }
        /* everything else becomes ASCII text: plain characters, entities, UTF-8 */
        char tmp[2];
        const char *text;
        int n = 1;
        if (c == '&' && !plain) { text = entity(p + 1, &n, tmp); n++; if (n == 1) text = "&"; }
        else if ((u8)c >= 0x80) text = uni_ascii(utf8_decode(p, &n), tmp);
        else { tmp[0] = c; tmp[1] = 0; text = (u8)c >= 32 ? tmp : ""; }
        if (!wl && *text && *text != ' ') word_glue = prev && !isspace((u8)prev);
        for (; *text; text++) {
            if (*text == ' ') FLUSH();
            else if (wl < 255) word[wl++] = *text;
        }
        p += n;
        prev = 'x';
    }
    FLUSH();
#undef FLUSH
}

/* ---------------------------------------------------------------- layout */
/* The UI face for a word: bold and heading (large) combine. */
static int style_face(u8 style)
{
    bool bold = style & ST_BOLD, large = style & ST_HEADING;
    return large ? (bold ? UI_FACE_LARGE_BOLD : UI_FACE_LARGE) : (bold ? UI_FACE_BOLD : UI_FACE_REG);
}

static int word_w(struct web *b, struct item *it)
{
    char tmp[256];
    memcpy(tmp, b->text + it->off, it->len);
    tmp[it->len] = 0;
    if (it->style & ST_PRE) return (int)it->len * FONT_W;
    return gfx_ui_face_width(tmp, style_face(it->style));
}

/* Centres or right-aligns each visual line (a run of runs with the same line number). */
static void align_lines(struct web *b, int width)
{
    int i = 0;
    while (i < b->nruns) {
        int line = b->runs[i].line, end = 0, j = i;
        for (; j < b->nruns && b->runs[j].line == line; j++) end = MAX(end, b->runs[j].x + b->runs[j].w);
        int free = width - end;
        if (b->runs[i].align && free > 0) {
            int dx = b->runs[i].align == 1 ? free / 2 : free;
            for (int k = i; k < j; k++) b->runs[k].x += dx;
        }
        i = j;
    }
}

static struct run *new_run(struct web *b, int x, int line, int w, const struct item *it)
{
    if (b->nruns == b->rcap) { b->rcap = b->rcap ? b->rcap * 2 : 512; b->runs = krealloc(b->runs, b->rcap * sizeof *b->runs); }
    struct run *r = &b->runs[b->nruns++];
    r->x = x; r->line = line; r->w = w;
    r->off = it->off; r->len = it->len; r->style = it->style; r->align = it->align;
    r->link = it->link; r->color = it->color;
    return r;
}

static void layout(window_t *w)
{
    struct web *b = w->app;
    int width = b->content.w - 2 * MARGIN - 16;
    if (width == b->laid_w) return;
    b->laid_w = width;
    b->nruns = 0;
    int x = 0, line = 0, space = gfx_ui_width(" ");
    bool first = true;                              /* nothing placed on this line yet */
    bool after_marker = false;
    for (int i = 0; i < b->nitems; i++) {
        struct item *it = &b->items[i];
        int base = it->indent * INDENT_PX;          /* where lines of this block start */
        if (it->type == IT_BREAK) { line++; first = true; continue; }
        if (it->type == IT_PARA) { line += 2; first = true; continue; }
        if (it->type == IT_RULE) { new_run(b, base, line, MAX(0, width - base), it); line++; first = true; continue; }
        if (it->type == IT_GAP) { if (!first) x += 16; continue; }       /* between table cells */
        int ww = word_w(b, it);
        int gap = first ? 0 : (it->glue && !after_marker ? 0 : space);
        after_marker = false;
        if (it->style & ST_MARKER) {                /* a bullet or number hangs left of the text */
            x = MAX(0, base - ww - 6);
            new_run(b, x, line, ww, it);
            x = base - space;
            first = false;
            after_marker = true;
            continue;
        }
        if (first) x = base;
        else if (x + gap + ww > width) { line++; x = base; gap = 0; }
        x += gap;
        new_run(b, x, line, ww, it);
        x += ww;
        first = false;
    }
    align_lines(b, width);
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
        if (r->style & ST_RULE) {                       /* <hr> */
            gfx_hline(s, x, y + 8, r->w, RGB(140, 140, 150));
            gfx_hline(s, x, y + 9, r->w, RGB(235, 235, 240));
            continue;
        }
        char tmp[256];
        memcpy(tmp, b->text + r->off, r->len);
        tmp[r->len] = 0;
        /* a CSS colour wins; links are blue (red while hovered) and text is black */
        u32 col = (r->style & ST_COLOR) ? r->color : r->link >= 0 ? (r->link == b->hover_link ? RGB(200, 40, 40) : RGB(20, 70, 200)) : C_TEXT;
        int face = style_face(r->style), tw, fh = gfx_ui_face_height(face);
        if (r->style & ST_PRE) { tw = gfx_text(s, x, y, tmp, col, COLOR_NONE); fh = FONT_H; }
        else if (r->style & ST_ITALIC) tw = gfx_ui_face_text_italic(s, x, y, tmp, face, col);
        else tw = gfx_ui_face_text(s, x, y, tmp, face, col);
        if (r->link >= 0 || (r->style & ST_UNDERLINE)) gfx_hline(s, x, y + fh - 1, tw, col);
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
    "<p>This is a simple text-mode browser for <b>http://</b> and <b>https://</b> pages. Secure pages "
    "are encrypted with TLS 1.3, but NimbusOS does not check the site's certificate, so it cannot prove "
    "who it is talking to. Type an address above and press Enter, or type words to search the web "
    "with DuckDuckGo.</p>"
    "<h2>Try</h2><ul>"
    "<li><a href=\"https://html.duckduckgo.com/html/?q=nimbusos\">Search the web</a> (or just type words in the address bar)</li>"
    "<li><a href=\"http://10.0.2.2:8080/\">http://10.0.2.2:8080/</a> - a web server on your host computer "
    "(e.g. run <b>python3 -m http.server 8080</b>; QEMU's user network maps 10.0.2.2 to the host)</li>"
    "<li><a href=\"http://info.cern.ch/\">http://info.cern.ch/</a> - the first website (1991)</li>"
    "<li><a href=\"https://example.com/\">https://example.com/</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">https://en.wikipedia.org/wiki/Operating_system</a></li>"
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

/* Percent-encodes search words for a query string: spaces become '+'. */
static void url_encode(const char *in, char *out, int max)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (; *in && o < max - 4; in++) {
        u8 c = (u8)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* Text typed in the address bar: an address has no spaces and a dot, colon or slash
 * ("example.com", "10.0.2.2:8080/x"); anything else is a search. */
static bool looks_like_address(const char *s)
{
    bool mark = false;
    for (; *s; s++) {
        if (*s == ' ') return false;
        if (*s == '.' || *s == ':' || *s == '/') mark = true;
    }
    return mark;
}

/* Percent-decodes up to the next '&' (a query parameter's value). '+' stays '+'. */
static void pct_decode(const char *in, char *out, int max)
{
    int o = 0;
    for (; *in && *in != '&' && o < max - 1; in++) {
        if (*in == '%' && hex_digit(in[1]) >= 0 && hex_digit(in[2]) >= 0) { out[o++] = (char)(hex_digit(in[1]) * 16 + hex_digit(in[2])); in += 2; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

/* DuckDuckGo wraps each result link in /l/?uddg=<encoded target>: go straight to the target. */
static void unwrap_redirect(char *url, int max)
{
    if (!strstr(url, "duckduckgo.com/l/")) return;
    const char *p = strstr(url, "uddg=");
    if (!p) return;
    char target[512];
    pct_decode(p + 5, target, sizeof target);
    if (!strncmp(target, "http://", 7) || !strncmp(target, "https://", 8)) strlcpy(url, target, max);
}

/* Removes "." and ".." segments from the path of an absolute URL (the query is kept). */
static void normalize_path(char *url, int max)
{
    char *p = strstr(url, "://");
    if (!p) return;
    char *path = strchr(p + 3, '/');
    if (!path) return;
    char tail[256];
    tail[0] = 0;
    char *q = strchr(path, '?');
    if (q) { strlcpy(tail, q, sizeof tail); *q = 0; }
    char out[256];
    out[0] = 0;
    bool slash = false;
    for (const char *s = path; *s == '/';) {
        s++;
        const char *e = s;
        while (*e && *e != '/') e++;
        int n = e - s;
        slash = false;
        if (n == 0 || (n == 1 && s[0] == '.')) slash = true;
        else if (n == 2 && s[0] == '.' && s[1] == '.') {
            char *last = strrchr(out, '/');
            if (last) *last = 0;
            slash = true;
        } else if ((int)strlen(out) + n + 2 < (int)sizeof out) {
            int ol = strlen(out);
            out[ol++] = '/';
            memcpy(out + ol, s, n);
            out[ol + n] = 0;
        }
        s = e;
    }
    if (!out[0]) strcpy(out, "/");
    else if (slash) strlcat(out, "/", sizeof out);
    int room = max - (int)(path - url);
    strlcpy(path, out, room);
    strlcat(url, tail, max);
}

static void navigate(window_t *w, const char *url, bool push)
{
    struct web *b = w->app;
    char u[256];
    strlcpy(u, url, sizeof u);
    if (strncmp(u, "http://", 7) && strncmp(u, "about:", 6) && strncmp(u, "https://", 8)) {
        if (looks_like_address(url)) snprintf(u, sizeof u, "http://%s", url);
        else {                                      /* search words */
            char q[200];
            url_encode(url, q, sizeof q);
            snprintf(u, sizeof u, "%s%s", settings_get("search_url", SEARCH_URL), q);
        }
    }
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
    if (b->job) { netjob_cancel(b->job); netjob_release(b->job); }
    b->job = netjob_fetch(u);
    snprintf(b->status, sizeof b->status, "Loading %s ...", u);
    klog("[web ] loading %s\n", u);
    wm_set_timer(w, 50);
    paint(w);
}

static void resolve_raw(struct web *b, const char *href, char *out, int max)
{
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8) || !strncmp(href, "about:", 6)) { strlcpy(out, href, max); return; }
    if (href[0] == '#') { strlcpy(out, b->url, max); return; }          /* same page */
    bool secure = !strncmp(b->url, "https://", 8);
    if (!strncmp(href, "//", 2)) { snprintf(out, max, "%s:%s", secure ? "https" : "http", href); return; }
    /* base = scheme://host[:port] and the directory of the current path (without its query) */
    char base[256];
    strlcpy(base, b->url, sizeof base);
    char *qm = strchr(base, '?');
    if (qm) *qm = 0;
    if (href[0] == '?') { snprintf(out, max, "%s%s", base, href); return; }
    char *host_end = strchr(base + (secure ? 8 : 7), '/');
    if (href[0] == '/') { if (host_end) *host_end = 0; snprintf(out, max, "%s%s", base, href); return; }
    if (!host_end) strlcat(base, "/", sizeof base);
    else { char *last = strrchr(base, '/'); last[1] = 0; }
    snprintf(out, max, "%s%s", base, href);
}

/* The absolute URL of a link: relative paths, ?query, //host and ../ resolved, #fragments
 * dropped, and search-engine redirect wrappers removed. */
static void resolve_link(struct web *b, const char *href, char *out, int max)
{
    resolve_raw(b, href, out, max);
    char *frag = strchr(out, '#');
    if (frag) *frag = 0;
    normalize_path(out, max);
    unwrap_redirect(out, max);
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
        if (!strncmp(b->url, "https://", 8))
            snprintf(b->status, sizeof b->status, "Done - encrypted, certificate not verified - HTTP %d, %u bytes, %d links (%s)",
                     status, len, b->nlinks, http_last_security());
        else
            snprintf(b->status, sizeof b->status, "Done - HTTP %d, %u bytes, %d links - not encrypted", status, len, b->nlinks);
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
        kfree(b->text); kfree(b->items); kfree(b->runs); kfree(b->rules); kfree(b->css); kfree(b);
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
