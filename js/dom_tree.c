/* NimbusOS JavaScript engine - DOM tree: HTML parser, serializer, tree operations, selectors. */
#include "js/dom_int.h"

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

int dn_is(dnode *n, const char *tag) { return n && n->type == DN_ELEMENT && strcmp(n->tag->s, tag) == 0; }

/* ---- text escaping ----------------------------------------------------------------------------------------- */

static void put_utf8(sbuf *o, unsigned cp)
{
    char t[4]; unsigned n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); n = 4; }
    sb_add(o, t, n);
}

void html_decode(const char *raw, unsigned len, sbuf *out)
{
    static const struct { const char *name; unsigned cp; } ents[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
        {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
        {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"times", 0xD7}, {0, 0}
    };
    for (unsigned i = 0; i < len; i++) {
        if (raw[i] != '&') { sb_addc(out, raw[i]); continue; }
        unsigned j = i + 1;
        if (j < len && raw[j] == '#') {
            j++;
            unsigned cp = 0; int hex = 0, nd = 0;
            if (j < len && (raw[j] == 'x' || raw[j] == 'X')) { hex = 1; j++; }
            while (j < len && nd < 8) {
                int c = raw[j], v = c >= '0' && c <= '9' ? c - '0' : hex && lc(c) >= 'a' && lc(c) <= 'f' ? lc(c) - 'a' + 10 : -1;
                if (v < 0) break;
                cp = cp * (hex ? 16u : 10u) + (unsigned)v; j++; nd++;
            }
            if (nd && cp && cp < 0x110000) { put_utf8(out, cp); i = (j < len && raw[j] == ';') ? j : j - 1; continue; }
        } else {
            unsigned k = j;
            while (k < len && k - j < 10 && ((raw[k] >= 'a' && raw[k] <= 'z') || (raw[k] >= 'A' && raw[k] <= 'Z') || (raw[k] >= '0' && raw[k] <= '9'))) k++;
            if (k < len && raw[k] == ';') {
                int found = 0;
                for (int e = 0; ents[e].name; e++)
                    if (strlen(ents[e].name) == k - j && memcmp(ents[e].name, raw + j, k - j) == 0) { put_utf8(out, ents[e].cp); found = 1; break; }
                if (found) { i = k; continue; }
            }
        }
        sb_addc(out, '&');
    }
}

void html_encode(const char *s, unsigned len, sbuf *out, int attr)
{
    for (unsigned i = 0; i < len; i++) {
        switch (s[i]) {
        case '&': sb_adds(out, "&amp;"); break;
        case '<': sb_adds(out, "&lt;"); break;
        case '>': sb_adds(out, "&gt;"); break;
        case '"': if (attr) sb_adds(out, "&quot;"); else sb_addc(out, '"'); break;
        default: sb_addc(out, s[i]);
        }
    }
}

/* ---- nodes and attributes ---------------------------------------------------------------------------------- */

static char *dup_n(const char *s, unsigned n)
{
    char *p = (char *)JS_MALLOC((size_t)n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

dnode *dn_new(dompage *d, int type, jstr *tag)
{
    dnode *n = (dnode *)JS_MALLOC(sizeof(dnode));
    if (!n) { d->rt->abort_msg = "out of memory"; static dnode dummy; return &dummy; }
    memset(n, 0, sizeof *n);
    n->type = (unsigned char)type;
    n->tag = tag;
    if (d->nnodes == d->ncap) {
        int nc = d->ncap ? d->ncap * 2 : 256;
        dnode **nn = (dnode **)JS_REALLOC(d->nodes, (size_t)nc * sizeof(dnode *));
        if (!nn) { d->rt->abort_msg = "out of memory"; JS_FREE(n); static dnode dummy2; return &dummy2; }
        d->nodes = nn; d->ncap = nc;
    }
    if (d->nnodes > 40000 && !d->rt->abort_msg) d->rt->abort_msg = "the page created too many elements";
    d->nodes[d->nnodes++] = n;
    n->id = d->nnodes;
    return n;
}

dattr *dn_attr(dnode *n, jstr *name)
{
    for (int i = 0; i < n->nattrs; i++) if (n->attrs[i].name == name) return &n->attrs[i];
    return NULL;
}

const char *dn_attr_raw(dnode *n, const char *name)
{
    for (int i = 0; i < n->nattrs; i++) if (strcmp(n->attrs[i].name->s, name) == 0) return n->attrs[i].val ? n->attrs[i].val : "";
    return NULL;
}

void dn_set_attr_raw(dompage *d, dnode *n, jstr *name, const char *raw, unsigned len)
{
    dattr *a = dn_attr(n, name);
    char *v = dup_n(raw, len);
    if (!v) { d->rt->abort_msg = "out of memory"; return; }
    if (a) { JS_FREE(a->val); a->val = v; }
    else {
        if (n->nattrs == n->acap) {
            int nc = n->acap ? n->acap * 2 : 4;
            dattr *na = (dattr *)JS_REALLOC(n->attrs, (size_t)nc * sizeof(dattr));
            if (!na) { JS_FREE(v); d->rt->abort_msg = "out of memory"; return; }
            n->attrs = na; n->acap = nc;
        }
        n->attrs[n->nattrs].name = name;
        n->attrs[n->nattrs].val = v;
        n->nattrs++;
    }
    d->dirty = 1;
}

void dn_del_attr(dompage *d, dnode *n, jstr *name)
{
    for (int i = 0; i < n->nattrs; i++) {
        if (n->attrs[i].name != name) continue;
        JS_FREE(n->attrs[i].val);
        memmove(&n->attrs[i], &n->attrs[i + 1], (size_t)(n->nattrs - i - 1) * sizeof(dattr));
        n->nattrs--;
        d->dirty = 1;
        return;
    }
}

void dn_set_text(dompage *d, dnode *n, const char *raw, unsigned len)
{
    char *t = dup_n(raw, len);
    if (!t) { d->rt->abort_msg = "out of memory"; return; }
    if (n->text) JS_FREE(n->text);
    n->text = t; n->tlen = len;
    d->dirty = 1;
}

/* ---- tree operations --------------------------------------------------------------------------------------- */

void dn_detach(dompage *d, dnode *n)
{
    if (!n->parent) return;
    if (n->prev) n->prev->next = n->next; else n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev; else n->parent->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    d->dirty = 1;
}

void dn_insert_before(dompage *d, dnode *parent, dnode *child, dnode *ref)
{
    if (child == ref) return;
    for (dnode *a = parent; a; a = a->parent) if (a == child) return;            /* would make a cycle */
    if (child->type == DN_FRAGMENT) {
        while (child->first) { dnode *c = child->first; dn_detach(d, c); dn_insert_before(d, parent, c, ref); }
        return;
    }
    dn_detach(d, child);
    child->parent = parent;
    if (ref && ref->parent == parent) {
        child->next = ref; child->prev = ref->prev;
        if (ref->prev) ref->prev->next = child; else parent->first = child;
        ref->prev = child;
    } else {
        child->prev = parent->last; child->next = NULL;
        if (parent->last) parent->last->next = child; else parent->first = child;
        parent->last = child;
    }
    d->dirty = 1;
}

void dn_append(dompage *d, dnode *parent, dnode *child) { dn_insert_before(d, parent, child, NULL); }

void dn_clear_children(dompage *d, dnode *n)
{
    while (n->first) dn_detach(d, n->first);
}

dnode *dn_clone(dompage *d, dnode *n, int deep)
{
    dnode *c = dn_new(d, n->type, n->tag);
    for (int i = 0; i < n->nattrs; i++) dn_set_attr_raw(d, c, n->attrs[i].name, n->attrs[i].val, (unsigned)strlen(n->attrs[i].val));
    if (n->text) dn_set_text(d, c, n->text, n->tlen);
    if (deep) for (dnode *k = n->first; k; k = k->next) dn_append(d, c, dn_clone(d, k, 1));
    return c;
}

void dn_text_content(dnode *n, sbuf *plain)
{
    if (n->type == DN_TEXT) { if (n->text) html_decode(n->text, n->tlen, plain); return; }
    for (dnode *k = n->first; k; k = k->next) dn_text_content(k, plain);
}

/* ---- parser ------------------------------------------------------------------------------------------------ */

static int void_tag(const char *t)
{
    static const char *v[] = { "br", "hr", "img", "meta", "link", "input", "wbr", "col", "area", "base", "embed", "param", "source", "track", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(t, v[i])) return 1;
    return 0;
}

static int raw_text_tag(const char *t) { return !strcmp(t, "script") || !strcmp(t, "style") || !strcmp(t, "textarea") || !strcmp(t, "title"); }

static int block_tag(const char *t)
{
    static const char *v[] = { "div", "p", "ul", "ol", "table", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "blockquote", "form", "hr", "section", "article", "header", "footer", "nav", "dl", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(t, v[i])) return 1;
    return 0;
}

static void add_text(dompage *d, dnode *parent, const char *s, unsigned len)
{
    if (!len) return;
    if (parent->last && parent->last->type == DN_TEXT) {                  /* merge with the previous text */
        dnode *t = parent->last;
        char *nt = (char *)JS_REALLOC(t->text, (size_t)t->tlen + len + 1);
        if (!nt) { d->rt->abort_msg = "out of memory"; return; }
        memcpy(nt + t->tlen, s, len);
        t->tlen += len; nt[t->tlen] = 0; t->text = nt;
        return;
    }
    dnode *t = dn_new(d, DN_TEXT, atom_c(d->rt, "#text"));
    dn_set_text(d, t, s, len);
    dn_append(d, parent, t);
}

/* implied end tags: <li> closes an open <li>, <p> closes before blocks, <td> before <td>, ... */
static dnode *auto_close(dnode *cur, dnode *limit, const char *tag)
{
    for (dnode *c = cur; c && c != limit; c = c->parent) {
        const char *t = c->tag->s;
        if (!strcmp(tag, "li")) { if (!strcmp(t, "li")) return c->parent; if (!strcmp(t, "ul") || !strcmp(t, "ol")) return cur; }
        else if (!strcmp(tag, "dt") || !strcmp(tag, "dd")) { if (!strcmp(t, "dt") || !strcmp(t, "dd")) return c->parent; if (!strcmp(t, "dl")) return cur; }
        else if (!strcmp(tag, "td") || !strcmp(tag, "th")) { if (!strcmp(t, "td") || !strcmp(t, "th")) return c->parent; if (!strcmp(t, "tr") || !strcmp(t, "table")) return cur; }
        else if (!strcmp(tag, "tr")) { if (!strcmp(t, "tr")) return c->parent; if (!strcmp(t, "table") || !strcmp(t, "tbody") || !strcmp(t, "thead")) return cur; }
        else if (!strcmp(tag, "option")) { if (!strcmp(t, "option")) return c->parent; if (!strcmp(t, "select")) return cur; }
        else if (block_tag(tag)) { if (!strcmp(t, "p")) return c->parent; }
        else return cur;
    }
    return cur;
}

void dn_parse(dompage *d, dnode *parent, const char *h, unsigned len)
{
    js_rt *rt = d->rt;
    dnode *cur = parent;
    unsigned i = 0;
    while (i < len && !rt->abort_msg) {
        if (h[i] != '<' || i + 1 >= len) {
            unsigned j = i + 1;
            while (j < len && !(h[j] == '<' && j + 1 < len && (h[j + 1] == '/' || h[j + 1] == '!' || h[j + 1] == '?' || (lc(h[j + 1]) >= 'a' && lc(h[j + 1]) <= 'z')))) j++;
            add_text(d, cur, h + i, j - i);
            i = j;
            continue;
        }
        if (len - i >= 4 && !memcmp(h + i, "<!--", 4)) {
            unsigned j = i + 4;
            while (j + 2 < len && memcmp(h + j, "-->", 3)) j++;
            i = j + 3 > len ? len : j + 3;
            continue;
        }
        if (h[i + 1] == '!' || h[i + 1] == '?') { while (i < len && h[i] != '>') i++; i++; continue; }
        if (h[i + 1] == '/') {
            unsigned j = i + 2, s = j;
            while (j < len && h[j] != '>' && !is_space(h[j])) j++;
            char name[40]; unsigned nl = j - s < 39 ? j - s : 39;
            for (unsigned k = 0; k < nl; k++) name[k] = (char)lc(h[s + k]);
            name[nl] = 0;
            while (j < len && h[j] != '>') j++;
            i = j + 1;
            for (dnode *c = cur; c && c != parent; c = c->parent)
                if (c->type == DN_ELEMENT && !strcmp(c->tag->s, name)) { cur = c->parent; break; }
            continue;
        }
        if (!((lc(h[i + 1]) >= 'a' && lc(h[i + 1]) <= 'z'))) { add_text(d, cur, h + i, 1); i++; continue; }
        /* a start tag */
        unsigned j = i + 1, s = j;
        while (j < len && !is_space(h[j]) && h[j] != '>' && h[j] != '/') j++;
        char name[40]; unsigned nl = j - s < 39 ? j - s : 39;
        for (unsigned k = 0; k < nl; k++) name[k] = (char)lc(h[s + k]);
        name[nl] = 0;
        cur = auto_close(cur, parent, name);
        dnode *el = dn_new(d, DN_ELEMENT, atom_c(rt, name));
        int self_close = 0;
        for (;;) {
            while (j < len && is_space(h[j])) j++;
            if (j >= len) break;
            if (h[j] == '>') { j++; break; }
            if (h[j] == '/') { if (j + 1 < len && h[j + 1] == '>') { self_close = 1; j += 2; break; } j++; continue; }
            unsigned as = j;
            while (j < len && !is_space(h[j]) && h[j] != '=' && h[j] != '>' && !(h[j] == '/' && j + 1 < len && h[j + 1] == '>')) j++;
            char an[48]; unsigned al = j - as < 47 ? j - as : 47;
            for (unsigned k = 0; k < al; k++) an[k] = (char)lc(h[as + k]);
            an[al] = 0;
            while (j < len && is_space(h[j])) j++;
            sbuf val; sb_init(&val);
            if (j < len && h[j] == '=') {
                j++;
                while (j < len && is_space(h[j])) j++;
                if (j < len && (h[j] == '"' || h[j] == '\'')) {
                    char q = h[j++];
                    while (j < len && h[j] != q) { if (h[j] == '"') sb_adds(&val, "&quot;"); else sb_addc(&val, h[j]); j++; }
                    if (j < len) j++;
                } else {
                    while (j < len && !is_space(h[j]) && h[j] != '>') { if (h[j] == '"') sb_adds(&val, "&quot;"); else sb_addc(&val, h[j]); j++; }
                }
            }
            if (al) {
                jstr *a = atom_c(rt, an);
                if (!dn_attr(el, a)) dn_set_attr_raw(d, el, a, val.p ? val.p : "", val.len);
            }
            sb_free(&val);
        }
        i = j;
        dn_append(d, cur, el);
        if (void_tag(name) || self_close) continue;
        if (raw_text_tag(name)) {                                         /* content up to the matching end tag */
            unsigned k = i, nlen = (unsigned)strlen(name);
            while (k + 2 + nlen <= len) {
                if (h[k] == '<' && h[k + 1] == '/') {
                    unsigned m = 0;
                    while (m < nlen && lc(h[k + 2 + m]) == name[m]) m++;
                    if (m == nlen) break;
                }
                k++;
            }
            if (k + 2 + nlen > len) k = len;
            if (k > i) { dnode *t = dn_new(d, DN_TEXT, atom_c(rt, "#text")); dn_set_text(d, t, h + i, k - i); dn_append(d, el, t); }
            i = k;
            while (i < len && h[i] != '>') i++;
            i++;
            continue;
        }
        cur = el;
    }
}

/* ---- serializer -------------------------------------------------------------------------------------------- */

int dn_has_click(dompage *d, dnode *n)
{
    if (n->type != DN_ELEMENT) return 0;
    if (dn_attr_raw(n, "onclick")) return 1;
    for (int i = 0; i < n->nls; i++) if (!strcmp(n->ls[i].type->s, "click")) return 1;
    if (n->wrapper) {
        prop *p = prop_find(n->wrapper, atom_c(d->rt, "onclick"));
        if (p && !(p->flags & PF_ACCESSOR) && is_callable(p->val)) return 1;
    }
    if (dn_is(n, "a")) {
        const char *h = dn_attr_raw(n, "href");
        if (h && !strncmp(h, "javascript:", 11)) return 1;
    }
    return 0;
}

static void put_num(sbuf *o, int v)
{
    char b[16]; int n = num_fmt(num_from_int(v), b);
    sb_add(o, b, (unsigned)n);
}

static void ser(dompage *d, dnode *n, sbuf *o, int render)
{
    if (n->type == DN_TEXT) { if (n->text) sb_add(o, n->text, n->tlen); return; }
    if (n->type != DN_ELEMENT) { for (dnode *k = n->first; k; k = k->next) ser(d, k, o, render); return; }
    const char *tag = n->tag->s;
    if (render && !strcmp(tag, "script")) return;
    int click = render && dn_has_click(d, n);
    int anchor = !strcmp(tag, "a");
    int field = !strcmp(tag, "input") || !strcmp(tag, "button") || !strcmp(tag, "textarea") || !strcmp(tag, "select");
    sb_addc(o, '<'); sb_adds(o, tag);
    for (int i = 0; i < n->nattrs; i++) {
        const char *an = n->attrs[i].name->s;
        if (render && anchor && click && !strcmp(an, "href")) continue;
        sb_addc(o, ' '); sb_adds(o, an);
        sb_adds(o, "=\""); sb_adds(o, n->attrs[i].val); sb_addc(o, '"');
    }
    if (render && (field || anchor || click || !strcmp(tag, "form"))) { sb_adds(o, " data-nid=\""); put_num(o, n->id); sb_addc(o, '"'); }
    if (render && anchor && click) { sb_adds(o, " href=\"nimbus-js:"); put_num(o, n->id); sb_addc(o, '"'); }
    sb_addc(o, '>');
    if (void_tag(tag)) return;
    int wrap = click && !anchor && !field;
    if (wrap) { sb_adds(o, "<a href=\"nimbus-js:"); put_num(o, n->id); sb_adds(o, "\">"); }
    for (dnode *k = n->first; k; k = k->next) ser(d, k, o, render);
    if (wrap) sb_adds(o, "</a>");
    sb_adds(o, "</"); sb_adds(o, tag); sb_addc(o, '>');
}

void dn_serialize(dompage *d, dnode *n, sbuf *out, int inner_only, int for_render)
{
    if (inner_only) { for (dnode *k = n->first; k; k = k->next) ser(d, k, out, for_render); }
    else ser(d, n, out, for_render);
}

/* ---- selectors --------------------------------------------------------------------------------------------- */

struct attrsel { char name[24], op, val[56]; };
struct comp {
    char tag[32], id[56], cls[4][32];
    int ncls, nat, first_child, last_child, checked, nth;
    struct attrsel at[3];
    char comb;                                  /* relation to the compound on its left: ' ' '>' '+' '~' */
};
#define MAXCOMP 8

static int name_char(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }

/* Parses one comma-free complex selector from *ps. Returns the number of compounds, or -1 on a syntax problem. */
static int parse_complex(const char **ps, struct comp *cs)
{
    const char *p = *ps;
    int n = 0;
    char comb = 0;
    for (;;) {
        while (is_space(*p)) p++;
        if (!*p || *p == ',') break;
        if (n == MAXCOMP) return -1;
        struct comp *c = &cs[n];
        memset(c, 0, sizeof *c);
        c->comb = comb;
        int any = 0;
        if (*p == '*') { strcpy(c->tag, "*"); p++; any = 1; }
        else { int k = 0; while (name_char(*p)) { if (k < 31) c->tag[k++] = (char)lc(*p); p++; any = 1; } }
        for (;;) {
            if (*p == '#') { p++; int k = 0; while (name_char(*p)) { if (k < 55) c->id[k++] = *p; p++; } any = 1; }
            else if (*p == '.') { p++; int k = 0; if (c->ncls < 4) { while (name_char(*p)) { if (k < 31) c->cls[c->ncls][k++] = *p; p++; } c->ncls++; } any = 1; }
            else if (*p == '[') {
                p++;
                if (c->nat >= 3) return -1;
                struct attrsel *a = &c->at[c->nat++];
                int k = 0;
                while (name_char(*p) || *p == ':') { if (k < 23) a->name[k++] = (char)lc(*p); p++; }
                while (is_space(*p)) p++;
                if (*p == ']') { a->op = 0; p++; }
                else {
                    if (*p == '~' || *p == '^' || *p == '$' || *p == '*' || *p == '|') a->op = *p++; else a->op = '=';
                    if (*p == '=') p++; else return -1;
                    while (is_space(*p)) p++;
                    k = 0;
                    if (*p == '"' || *p == '\'') { char q = *p++; while (*p && *p != q) { if (k < 55) a->val[k++] = *p; p++; } if (*p) p++; }
                    else while (*p && *p != ']' && !is_space(*p)) { if (k < 55) a->val[k++] = *p; p++; }
                    while (is_space(*p)) p++;
                    if (*p == ']') p++; else return -1;
                }
                any = 1;
            }
            else if (*p == ':') {
                p++;
                char pn[24]; int k = 0;
                while (name_char(*p)) { if (k < 23) pn[k++] = (char)lc(*p); p++; }
                pn[k] = 0;
                if (!strcmp(pn, "first-child")) c->first_child = 1;
                else if (!strcmp(pn, "last-child")) c->last_child = 1;
                else if (!strcmp(pn, "checked")) c->checked = 1;
                else if (!strcmp(pn, "nth-child") && *p == '(') { p++; c->nth = 0; while (*p >= '0' && *p <= '9') c->nth = c->nth * 10 + (*p++ - '0'); while (*p && *p != ')') p++; if (*p) p++; }
                else return -1;
                any = 1;
            }
            else break;
        }
        if (!any) return -1;
        n++;
        while (is_space(*p)) p++;
        if (*p == '>' || *p == '+' || *p == '~') { comb = *p++; }
        else if (*p && *p != ',') comb = ' ';
        else break;
    }
    *ps = p;
    return n;
}

static int class_has(const char *cls, const char *want)
{
    size_t wl = strlen(want);
    for (const char *p = cls; *p; ) {
        while (is_space(*p)) p++;
        const char *s = p;
        while (*p && !is_space(*p)) p++;
        if ((size_t)(p - s) == wl && !memcmp(s, want, wl)) return 1;
    }
    return 0;
}

static int match_compound(dompage *d, dnode *n, struct comp *c)
{
    (void)d;
    if (n->type != DN_ELEMENT) return 0;
    if (c->tag[0] && strcmp(c->tag, "*") && strcmp(c->tag, n->tag->s)) return 0;
    if (c->id[0]) { const char *v = dn_attr_raw(n, "id"); if (!v || strcmp(v, c->id)) return 0; }
    if (c->ncls) {
        const char *v = dn_attr_raw(n, "class");
        if (!v) return 0;
        for (int i = 0; i < c->ncls; i++) if (!class_has(v, c->cls[i])) return 0;
    }
    for (int i = 0; i < c->nat; i++) {
        struct attrsel *a = &c->at[i];
        const char *raw = dn_attr_raw(n, a->name);
        if (!raw) return 0;
        if (!a->op) continue;
        sbuf dv; sb_init(&dv);
        html_decode(raw, (unsigned)strlen(raw), &dv);
        const char *v = dv.p ? dv.p : "";
        size_t vl = strlen(v), al = strlen(a->val);
        int ok = 0;
        switch (a->op) {
        case '=': ok = !strcmp(v, a->val); break;
        case '~': ok = class_has(v, a->val); break;
        case '^': ok = al && vl >= al && !memcmp(v, a->val, al); break;
        case '$': ok = al && vl >= al && !memcmp(v + vl - al, a->val, al); break;
        case '*': ok = al && strstr(v, a->val) != NULL; break;
        case '|': ok = !strcmp(v, a->val) || (vl > al && !memcmp(v, a->val, al) && v[al] == '-'); break;
        }
        sb_free(&dv);
        if (!ok) return 0;
    }
    if (c->checked && !dn_attr_raw(n, "checked")) return 0;
    if (c->first_child || c->last_child || c->nth) {
        int idx = 1, total = 0;
        for (dnode *k = n->parent ? n->parent->first : NULL; k; k = k->next) if (k->type == DN_ELEMENT) { total++; if (k == n) idx = total; }
        int count = total;
        if (c->first_child && idx != 1) return 0;
        if (c->last_child && idx != count) return 0;
        if (c->nth && idx != c->nth) return 0;
    }
    return 1;
}

static dnode *prev_el(dnode *n) { dnode *p = n->prev; while (p && p->type != DN_ELEMENT) p = p->prev; return p; }

static int match_from(dompage *d, dnode *n, struct comp *cs, int i)
{
    if (!match_compound(d, n, &cs[i])) return 0;
    if (i == 0) return 1;
    switch (cs[i].comb) {
    case '>': return n->parent && match_from(d, n->parent, cs, i - 1);
    case '+': { dnode *p = prev_el(n); return p && match_from(d, p, cs, i - 1); }
    case '~': for (dnode *p = prev_el(n); p; p = prev_el(p)) if (match_from(d, p, cs, i - 1)) return 1; return 0;
    default:  for (dnode *a = n->parent; a; a = a->parent) if (match_from(d, a, cs, i - 1)) return 1; return 0;
    }
}

int sel_matches(dompage *d, dnode *n, const char *sel)
{
    const char *p = sel;
    struct comp cs[MAXCOMP];
    for (;;) {
        int k = parse_complex(&p, cs);
        if (k < 0) return 0;
        if (k > 0 && match_from(d, n, cs, k - 1)) return 1;
        while (is_space(*p)) p++;
        if (*p != ',') break;
        p++;
    }
    return 0;
}

static void walk_query(dompage *d, dnode *n, const char *sel, int all, jsobj *out, dnode **first)
{
    for (dnode *k = n->first; k && !d->rt->abort_msg; k = k->next) {
        if (k->type != DN_ELEMENT) continue;
        if (sel_matches(d, k, sel)) {
            if (!all) { if (!*first) *first = k; return; }
            arr_push(d->rt, out, dn_wrap(d, k));
        }
        walk_query(d, k, sel, all, out, first);
        if (!all && *first) return;
    }
}

void sel_query(dompage *d, dnode *root, const char *sel, int all, jsobj *out, dnode **first)
{
    if (first) *first = NULL;
    dnode *f = NULL;
    walk_query(d, root, sel, all, out, first ? first : &f);
}

void dn_by_tag(dompage *d, dnode *root, const char *tag, jsobj *out)
{
    for (dnode *k = root->first; k; k = k->next) {
        if (k->type != DN_ELEMENT) continue;
        if (!strcmp(tag, "*") || !strcmp(k->tag->s, tag)) arr_push(d->rt, out, dn_wrap(d, k));
        dn_by_tag(d, k, tag, out);
    }
}

void dn_by_class(dompage *d, dnode *root, const char *cls, jsobj *out)
{
    for (dnode *k = root->first; k; k = k->next) {
        if (k->type != DN_ELEMENT) continue;
        const char *v = dn_attr_raw(k, "class");
        if (v && class_has(v, cls)) arr_push(d->rt, out, dn_wrap(d, k));
        dn_by_class(d, k, cls, out);
    }
}

dnode *dn_by_id(dompage *d, dnode *root, const char *id)
{
    for (dnode *k = root->first; k; k = k->next) {
        if (k->type != DN_ELEMENT) continue;
        const char *v = dn_attr_raw(k, "id");
        if (v && !strcmp(v, id)) return k;
        dnode *r = dn_by_id(d, k, id);
        if (r) return r;
    }
    return NULL;
}
