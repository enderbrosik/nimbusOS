/* NimbusOS JavaScript engine - heap cells: strings, atoms, objects, arrays, garbage collector. */
#include "js/js_int.h"

#define GC_MIN_THRESHOLD (256 * 1024)

/* ---- cells ------------------------------------------------------------------------------------------------- */

static void *cell_alloc(js_rt *rt, unsigned kind, unsigned size)
{
    if (rt->stack_base && !rt->in_gc && (rt->bytes > rt->gc_mark_bytes || (rt->gc_stress && ++rt->gc_stress_n >= rt->gc_stress))) {
        rt->gc_stress_n = 0;
        gc_run(rt);                                          /* collect in the middle of a script */
    }
    gcc *c = (gcc *)JS_MALLOC(size);
    if (!c) {
        if (!rt->abort_msg) rt->abort_msg = "out of memory";
        return kind == GK_STR ? (void *)rt->dummy_str : (void *)rt->dummy_obj;
    }
    memset(c, 0, size);
    c->kind = (unsigned char)kind;
    c->next = rt->heap;
    rt->heap = c;
    rt->bytes += size;
    if (rt->bytes > rt->max_bytes && !rt->abort_msg) rt->abort_msg = "out of memory";
    return c;
}

static unsigned hash_bytes(const char *s, unsigned n)
{
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

jstr *str_new(js_rt *rt, const char *s, unsigned len)
{
    jstr *r = (jstr *)cell_alloc(rt, GK_STR, (unsigned)(sizeof(jstr) + len));
    if (r == rt->dummy_str) return r;
    r->len = len;
    if (len) memcpy(r->s, s, len);
    r->s[len] = 0;
    r->hash = hash_bytes(r->s, len);
    return r;
}

jstr *str_cat(js_rt *rt, jstr *a, jstr *b)
{
    if (!a->len) return b;
    if (!b->len) return a;
    jstr *r = (jstr *)cell_alloc(rt, GK_STR, (unsigned)(sizeof(jstr) + a->len + b->len));
    if (r == rt->dummy_str) return r;
    r->len = a->len + b->len;
    memcpy(r->s, a->s, a->len);
    memcpy(r->s + a->len, b->s, b->len);
    r->s[r->len] = 0;
    r->hash = hash_bytes(r->s, r->len);
    return r;
}

int str_eq(jstr *a, jstr *b)
{
    return a == b || (a->len == b->len && a->hash == b->hash && memcmp(a->s, b->s, a->len) == 0);
}

/* ---- atoms: interned strings, compared by pointer ---------------------------------------------------------- */

static void atoms_grow(js_rt *rt)
{
    unsigned ncap = rt->atom_cap ? rt->atom_cap * 2 : 256;
    jstr **nt = (jstr **)JS_MALLOC(ncap * sizeof(jstr *));
    if (!nt) { rt->abort_msg = "out of memory"; return; }
    memset(nt, 0, ncap * sizeof(jstr *));
    for (unsigned i = 0; i < rt->atom_cap; i++) {
        jstr *s = rt->atoms[i];
        if (!s) continue;
        unsigned k = s->hash & (ncap - 1);
        while (nt[k]) k = (k + 1) & (ncap - 1);
        nt[k] = s;
    }
    JS_FREE(rt->atoms);
    rt->atoms = nt;
    rt->atom_cap = ncap;
}

jstr *atom_new(js_rt *rt, const char *s, unsigned len)
{
    if (rt->atom_count * 10 >= rt->atom_cap * 7) atoms_grow(rt);
    if (!rt->atom_cap) return rt->dummy_str;
    unsigned h = hash_bytes(s, len), k = h & (rt->atom_cap - 1);
    while (rt->atoms[k]) {
        jstr *e = rt->atoms[k];
        if (e->hash == h && e->len == len && memcmp(e->s, s, len) == 0) return e;
        k = (k + 1) & (rt->atom_cap - 1);
    }
    jstr *r = str_new(rt, s, len);
    if (r == rt->dummy_str) return r;
    r->atom = 1;
    rt->atoms[k] = r;
    rt->atom_count++;
    return r;
}

jstr *atom_c(js_rt *rt, const char *s) { return atom_new(rt, s, (unsigned)strlen(s)); }

jstr *to_atom(js_rt *rt, jstr *s) { return s->atom ? s : atom_new(rt, s->s, s->len); }

/* "12" -> 12 (canonical array index, below 2^31) */
int atom_index(jstr *a, unsigned *idx)
{
    if (!a->len || a->len > 10) return 0;
    if (a->s[0] == '0' && a->len > 1) return 0;
    unsigned long long v = 0;
    for (unsigned i = 0; i < a->len; i++) {
        if (a->s[i] < '0' || a->s[i] > '9') return 0;
        v = v * 10 + (unsigned)(a->s[i] - '0');
    }
    if (v >= 0x7FFFFFFFull) return 0;
    *idx = (unsigned)v;
    return 1;
}

/* ---- objects ----------------------------------------------------------------------------------------------- */

jsobj *obj_new(js_rt *rt, jsobj *proto, int cls)
{
    jsobj *o = (jsobj *)cell_alloc(rt, GK_OBJ, sizeof(jsobj));
    if (o == rt->dummy_obj) return o;
    o->cls = (unsigned char)cls;
    o->proto = proto;
    o->extensible = 1;
    o->bound_this = v_undef();
    o->thisv = v_undef();
    return o;
}

prop *prop_find(jsobj *o, jstr *key)
{
    for (int i = 0; i < o->np; i++)
        if (o->props[i].key == key) return &o->props[i];
    return NULL;
}

void prop_put(js_rt *rt, jsobj *o, jstr *key, jsval v, int flags)
{
    prop *p = prop_find(o, key);
    if (p) { p->val = v; return; }
    if (o->np == o->pcap) {
        int ncap = o->pcap ? o->pcap * 2 : 4;
        prop *np = (prop *)JS_REALLOC(o->props, (size_t)ncap * sizeof(prop));
        if (!np) { rt->abort_msg = "out of memory"; return; }
        rt->bytes += (unsigned long)(ncap - o->pcap) * sizeof(prop);
        o->props = np;
        o->pcap = ncap;
    }
    p = &o->props[o->np++];
    p->key = key;
    p->val = v;
    p->flags = (unsigned char)flags;
}

int prop_del(jsobj *o, jstr *key)
{
    for (int i = 0; i < o->np; i++) {
        if (o->props[i].key == key) {
            memmove(&o->props[i], &o->props[i + 1], (size_t)(o->np - i - 1) * sizeof(prop));
            o->np--;
            return 1;
        }
    }
    return 0;
}

jsobj *arr_new(js_rt *rt, unsigned n)
{
    jsobj *a = obj_new(rt, rt->proto_array, CL_ARRAY);
    if (a == rt->dummy_obj) return a;
    if (n) arr_set_len(rt, a, n);
    return a;
}

void arr_set_len(js_rt *rt, jsobj *a, unsigned n)
{
    if (n > a->acap) {
        unsigned ncap = a->acap ? a->acap : 4;
        while (ncap < n) ncap *= 2;
        if (ncap > 0x4000000u) { rt->abort_msg = "out of memory"; return; }
        jsval *ni = (jsval *)JS_REALLOC(a->items, (size_t)ncap * sizeof(jsval));
        if (!ni) { rt->abort_msg = "out of memory"; return; }
        rt->bytes += (unsigned long)(ncap - a->acap) * sizeof(jsval);
        if (rt->bytes > rt->max_bytes && !rt->abort_msg) rt->abort_msg = "out of memory";
        a->items = ni;
        a->acap = ncap;
    }
    for (unsigned i = a->alen; i < n; i++) a->items[i] = v_undef();
    a->alen = n;
}

void arr_push(js_rt *rt, jsobj *a, jsval v)
{
    unsigned n = a->alen;
    arr_set_len(rt, a, n + 1);
    if (a->alen == n + 1) a->items[n] = v;
}

/* ---- arena for AST nodes (freed with the runtime) --------------------------------------------------------- */

void *arena_alloc(js_rt *rt, unsigned n)
{
    n = (n + 7u) & ~7u;
    arena_blk *b = rt->arena;
    if (!b || b->used + n > b->size) {
        unsigned size = n > 32768 ? n : 32768;
        b = (arena_blk *)JS_MALLOC(sizeof(arena_blk) + size);
        if (!b) { rt->abort_msg = "out of memory"; return NULL; }
        b->next = rt->arena; b->used = 0; b->size = size;
        rt->arena = b;
        rt->bytes += size;
    }
    void *p = (char *)(b + 1) + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

/* ---- string builder ---------------------------------------------------------------------------------------- */

void sb_init(sbuf *b) { b->p = NULL; b->len = b->cap = 0; b->oom = 0; }

void sb_add(sbuf *b, const char *s, unsigned n)
{
    if (b->oom || !n) return;
    if (b->len + n + 1 > b->cap) {
        unsigned nc = b->cap ? b->cap : 64;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = (char *)JS_REALLOC(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sb_addc(sbuf *b, char c) { sb_add(b, &c, 1); }
void sb_adds(sbuf *b, const char *s) { sb_add(b, s, (unsigned)strlen(s)); }
void sb_free(sbuf *b) { if (b->p) JS_FREE(b->p); b->p = NULL; b->len = b->cap = 0; }

jstr *sb_str(js_rt *rt, sbuf *b)
{
    if (b->oom) { rt->abort_msg = "out of memory"; return rt->a_empty; }
    return str_new(rt, b->p ? b->p : "", b->len);
}

/* ---- garbage collector (mark and sweep; call only when no script is running) ------------------------------- */

typedef struct { gcc **p; unsigned n, cap; int oom; } gstack;

static void gpush(gstack *s, gcc *c)
{
    if (c->mark) return;
    c->mark = 1;
    if (c->kind == GK_STR) return;
    if (s->n == s->cap) {
        unsigned nc = s->cap ? s->cap * 2 : 256;
        gcc **np = (gcc **)JS_REALLOC(s->p, nc * sizeof(gcc *));
        if (!np) { s->oom = 1; return; }
        s->p = np; s->cap = nc;
    }
    s->p[s->n++] = c;
}

static void gval(gstack *s, jsval v)
{
    if (v.t == JT_STR) gpush(s, &v.u.s->h);
    else if (v.t == JT_OBJ) gpush(s, &v.u.o->h);
}

static void gobj(gstack *s, jsobj *o)
{
    if (o) gpush(s, &o->h);
}

void js_pin(js_rt *rt, jsval v)
{
    if (rt->npins == rt->pincap) {
        int nc = rt->pincap ? rt->pincap * 2 : 16;
        jsval *np = (jsval *)JS_REALLOC(rt->pins, (size_t)nc * sizeof(jsval));
        if (!np) { rt->abort_msg = "out of memory"; return; }
        rt->pins = np; rt->pincap = nc;
    }
    rt->pins[rt->npins++] = v;
}

/* ---- conservative scan of the C stack ----------------------------------------------------------------------
 * While a script runs, its temporaries (partly built arrays, scopes, operands) live only in C locals and registers.
 * Every word on the C stack that points into a cell (or into an object's property/item arrays) keeps it alive.
 * Words are collected, sorted, and each cell is looked up among them, so no index of the heap is needed. */

static void sift_down(unsigned long *a, long n, long i)
{
    for (;;) {
        long l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && a[l] > a[m]) m = l;
        if (r < n && a[r] > a[m]) m = r;
        if (m == i) return;
        unsigned long t = a[i]; a[i] = a[m]; a[m] = t;
        i = m;
    }
}

static void heap_sort(unsigned long *a, long n)
{
    for (long i = n / 2 - 1; i >= 0; i--) sift_down(a, n, i);
    for (long e = n - 1; e > 0; e--) { unsigned long t = a[0]; a[0] = a[e]; a[e] = t; sift_down(a, e, 0); }
}

/* is any candidate word inside [lo, hi)? */
static int any_in(const unsigned long *c, long n, unsigned long lo, unsigned long hi)
{
    long a = 0, b = n;
    while (a < b) { long m = (a + b) / 2; if (c[m] < lo) a = m + 1; else b = m; }
    return a < n && c[a] < hi;
}

static unsigned long *g_cands; static long g_ncands; static int g_scan_ok;

/* reads the raw stack on purpose, so address sanitizers must not check it */
static void __attribute__((noinline, no_sanitize_address)) scan_stack(js_rt *rt)
{
    __builtin_unwind_init();                                 /* callee-saved registers are spilled into this frame */
    volatile unsigned long marker = 0;
    char *lo = (char *)&marker, *hi = rt->stack_base;
    g_cands = NULL; g_ncands = 0; g_scan_ok = 1;
    if (!hi || hi <= lo) return;
    long words = (long)((hi - lo) / (long)sizeof(unsigned long));
    unsigned long *c = (unsigned long *)JS_MALLOC((size_t)words * sizeof(unsigned long));
    if (!c) { g_scan_ok = 0; return; }
    long n = 0;
    for (const unsigned long *w = (const unsigned long *)(((unsigned long)lo + 7) & ~7ul); (const char *)w < hi; w++)
        if (*w > 4096) c[n++] = *w;
    heap_sort(c, n);
    g_cands = c; g_ncands = n;
}

static void mark_from_stack(js_rt *rt, gstack *st)
{
    if (!rt->stack_base) return;
    scan_stack(rt);
    if (!g_scan_ok) { st->oom = 1; return; }
    for (gcc *c = rt->heap; c; c = c->next) {
        unsigned long a = (unsigned long)c;
        if (c->kind == GK_STR) {
            if (!c->mark && any_in(g_cands, g_ncands, a, a + sizeof(jstr) + ((jstr *)c)->len)) c->mark = 1;
        } else {
            jsobj *o = (jsobj *)c;
            int hit = any_in(g_cands, g_ncands, a, a + sizeof(jsobj));
            if (!hit && o->props) hit = any_in(g_cands, g_ncands, (unsigned long)o->props, (unsigned long)o->props + (unsigned long)o->pcap * sizeof(prop));
            if (!hit && o->items) hit = any_in(g_cands, g_ncands, (unsigned long)o->items, (unsigned long)o->items + (unsigned long)o->acap * sizeof(jsval));
            if (!hit && o->bound_args) hit = any_in(g_cands, g_ncands, (unsigned long)o->bound_args, (unsigned long)o->bound_args + (unsigned long)o->nbound * sizeof(jsval));
            if (hit) gpush(st, c);                            /* also marks everything it refers to */
        }
    }
    if (g_cands) JS_FREE(g_cands);
    g_cands = NULL;
}

void gc_run(js_rt *rt)
{
    gstack st = { NULL, 0, 0, 0 };
    if (rt->in_gc) return;
    rt->in_gc = 1;
    for (gcc *c = rt->heap; c; c = c->next) c->mark = 0;
    for (unsigned i = 0; i < rt->atom_cap; i++) if (rt->atoms[i]) rt->atoms[i]->h.mark = 1;
    rt->dummy_str->h.mark = 1;
    gobj(&st, rt->dummy_obj);
    gobj(&st, rt->global);
    gobj(&st, rt->proto_object); gobj(&st, rt->proto_function); gobj(&st, rt->proto_array);
    gobj(&st, rt->proto_string); gobj(&st, rt->proto_number); gobj(&st, rt->proto_boolean);
    gobj(&st, rt->proto_error); gobj(&st, rt->proto_date);
    gval(&st, rt->exc); gval(&st, rt->ret); gval(&st, rt->completion);
    for (int i = 0; i < rt->npins; i++) gval(&st, rt->pins[i]);
    mark_from_stack(rt, &st);
    while (st.n && !st.oom) {
        jsobj *o = (jsobj *)st.p[--st.n];
        gobj(&st, o->proto); gobj(&st, o->env); gobj(&st, o->outer); gobj(&st, o->bound_fn);
        for (int i = 0; i < o->np; i++) gval(&st, o->props[i].val);
        for (unsigned i = 0; i < o->alen; i++) gval(&st, o->items[i]);
        gval(&st, o->bound_this); gval(&st, o->thisv);
        for (int i = 0; i < o->nbound; i++) gval(&st, o->bound_args[i]);
    }
    if (st.p) JS_FREE(st.p);
    if (st.oom) { rt->in_gc = 0; return; }    /* could not finish marking: keep everything */
    gcc **pp = &rt->heap;
    while (*pp) {
        gcc *c = *pp;
        if (c->mark) { pp = &c->next; continue; }
        *pp = c->next;
        if (c->kind == GK_STR) {
            rt->bytes -= (unsigned long)(sizeof(jstr) + ((jstr *)c)->len);
        } else {
            jsobj *o = (jsobj *)c;
            rt->bytes -= sizeof(jsobj) + (unsigned long)o->pcap * sizeof(prop) + (unsigned long)o->acap * sizeof(jsval);
            if (o->props) JS_FREE(o->props);
            if (o->items) JS_FREE(o->items);
            if (o->bound_args) JS_FREE(o->bound_args);
        }
        JS_FREE(c);
    }
    rt->gc_mark_bytes = rt->bytes + GC_MIN_THRESHOLD > rt->bytes * 2 ? rt->bytes + GC_MIN_THRESHOLD : rt->bytes * 2;
    rt->in_gc = 0;
}
