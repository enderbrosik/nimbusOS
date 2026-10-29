/* NimbusOS JavaScript engine - DOM bindings: window, document, elements, events, timers, and the page API. */
#include "js/dom_int.h"

#define ARG(i) ((i) < nargs ? args[i] : v_undef())
#define U v_undef()
#define BAD() (NOVAL(rt))

static dompage *D(js_rt *rt) { return (dompage *)rt->dom; }
static jsval vnum(long long i) { return v_num(num_from_int(i)); }
static jsval vstr(js_rt *rt, const char *s) { return v_str(str_new(rt, s, (unsigned)strlen(s))); }

void dom_mark_dirty(dompage *d) { d->dirty = 1; }

/* ---- wrapping ---------------------------------------------------------------------------------------------- */

static jsobj *proto_document_of(dompage *d);

jsval dn_wrap(dompage *d, dnode *n)
{
    if (!n) return v_null();
    js_rt *rt = d->rt;
    if (!n->wrapper) {
        jsobj *o = obj_new(rt, n->type == DN_DOCUMENT ? proto_document_of(d) : d->proto_node, CL_OBJECT);
        prop_put(rt, o, d->a_n, vnum(n->id), PF_HIDDEN);
        n->wrapper = o;
        js_pin(rt, v_obj(o));
    }
    return v_obj(n->wrapper);
}

static dnode *nd(js_rt *rt, jsval v)
{
    dompage *d = D(rt);
    if (v.t != JT_OBJ) return NULL;
    prop *p = prop_find(v.u.o, d->a_n);
    if (!p || p->val.t != JT_NUM) return NULL;
    long long id = p->val.u.n / JS_SCALE;
    return id >= 1 && id <= d->nnodes ? d->nodes[id - 1] : NULL;
}

#define THIS_NODE() dnode *n = nd(rt, thisv); if (!n) return throw_error(rt, "TypeError", "Illegal invocation", NULL, NULL)

static dnode *arg_node(js_rt *rt, jsval v)
{
    dnode *n = nd(rt, v);
    if (!n) throw_error(rt, "TypeError", "parameter is not of type 'Node'", NULL, NULL);
    return n;
}

static jstr *lower_atom(js_rt *rt, jstr *s)
{
    char buf[64]; unsigned n = s->len < 63 ? s->len : 63;
    for (unsigned i = 0; i < n; i++) { char c = s->s[i]; buf[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
    return atom_new(rt, buf, n);
}

/* ---- reporting errors from callbacks ----------------------------------------------------------------------- */

static void host_log(dompage *d, const char *s, unsigned len)
{
    if (d->host.log) d->host.log(d->host.ud, s, (int)len);
}

static void report_failure(dompage *d)
{
    js_rt *rt = d->rt;
    if (rt->abort_msg) {
        sbuf b; sb_init(&b);
        sb_adds(&b, "Error: "); sb_adds(&b, rt->abort_msg);
        host_log(d, b.p, b.len);
        sb_free(&b);
        rt->abort_msg = NULL;
        rt->throwing = 0;
        return;
    }
    if (!rt->throwing) return;
    jsval ex = rt->exc;
    rt->throwing = 0;
    sbuf b; sb_init(&b);
    sb_adds(&b, "Uncaught ");
    if (ex.t == JT_OBJ && ex.u.o->cls == CL_ERROR) {
        jstr *ns = to_str(rt, get_prop(rt, ex, rt->a_name)), *ms = to_str(rt, get_prop(rt, ex, rt->a_message));
        sb_add(&b, ns->s, ns->len);
        if (ms->len) { sb_adds(&b, ": "); sb_add(&b, ms->s, ms->len); }
    } else inspect(rt, ex, &b, 0, 1);
    rt->throwing = 0;
    host_log(d, b.p, b.len);
    sb_free(&b);
}

static jsval call_safe(dompage *d, jsval fn, jsval thisv, jsval *args, int n)
{
    js_rt *rt = d->rt;
    if (d->depth <= 1) { rt->steps = 0; rt->depth = 0; }
    jsval r = call_fn(rt, fn, thisv, args, n);
    if (rt->throwing || rt->abort_msg) { report_failure(d); return v_undef(); }
    return r;
}

/* Compiles source text into a function taking `event` (for onclick="..." attributes, setTimeout("..."), javascript: links). */
static jsval compile_handler(dompage *d, const char *code, unsigned len)
{
    js_rt *rt = d->rt;
    sbuf b; sb_init(&b);
    sb_adds(&b, "(function(event){");
    sb_add(&b, code, len);
    sb_adds(&b, "\n})");
    jsval fn = v_undef();
    char err[200];
    int r = js_eval_value(rt, b.p, (int)b.len, &fn, err, sizeof err);
    sb_free(&b);
    if (r) { host_log(d, err, (unsigned)strlen(err)); return v_undef(); }
    return fn;
}

/* ---- events ------------------------------------------------------------------------------------------------ */

static jsobj *make_event(dompage *d, const char *type, int bubbles)
{
    js_rt *rt = d->rt;
    jsobj *ev = obj_new(rt, d->proto_event, CL_OBJECT);
    prop_put(rt, ev, atom_c(rt, "type"), vstr(rt, type), 0);
    prop_put(rt, ev, atom_c(rt, "bubbles"), v_bool(bubbles), 0);
    prop_put(rt, ev, atom_c(rt, "cancelable"), v_bool(1), 0);
    prop_put(rt, ev, atom_c(rt, "defaultPrevented"), v_bool(0), 0);
    prop_put(rt, ev, atom_c(rt, "target"), v_null(), 0);
    prop_put(rt, ev, atom_c(rt, "currentTarget"), v_null(), 0);
    prop_put(rt, ev, atom_c(rt, "timeStamp"), v_num(num_from_int(d->host.now_ms ? d->host.now_ms(d->host.ud) : 0)), 0);
    return ev;
}

static int ev_flag(js_rt *rt, jsobj *ev, const char *name)
{
    prop *p = prop_find(ev, atom_c(rt, name));
    return p && to_bool(p->val);
}

/* Calls the handlers of one node for an event: the on<type> property or attribute, then addEventListener listeners. */
static void fire_on(dompage *d, dnode *n, jsobj *ev, const char *type)
{
    js_rt *rt = d->rt;
    jsval w = dn_wrap(d, n);
    char pn[48];
    unsigned tl = (unsigned)strlen(type);
    if (tl > 40) return;
    pn[0] = 'o'; pn[1] = 'n'; memcpy(pn + 2, type, tl + 1);
    jstr *key = atom_c(rt, pn);
    prop_put(rt, ev, atom_c(rt, "currentTarget"), w, 0);
    jsval h = v_undef();
    prop *p = prop_find(n->wrapper, key);
    if (p && !(p->flags & PF_ACCESSOR)) h = p->val;
    else if (n->type == DN_ELEMENT) {
        const char *src = dn_attr_raw(n, pn);
        if (src) {
            sbuf dec; sb_init(&dec);
            html_decode(src, (unsigned)strlen(src), &dec);
            h = compile_handler(d, dec.p ? dec.p : "", dec.len);
            sb_free(&dec);
            prop_put(rt, n->wrapper, key, h, PF_HIDDEN);
        }
    }
    if (is_callable(h)) {
        jsval args[1] = { v_obj(ev) };
        jsval r = call_safe(d, h, w, args, 1);
        if (r.t == JT_BOOL && !r.u.b) prop_put(rt, ev, atom_c(rt, "defaultPrevented"), v_bool(1), 0);
    }
    for (int i = 0; i < n->nls; i++) {
        if (strcmp(n->ls[i].type->s, type)) continue;
        jsval args[1] = { v_obj(ev) };
        call_safe(d, n->ls[i].fn, w, args, 1);
        if (ev_flag(rt, ev, "__stopnow")) break;
    }
}

static void fire_window(dompage *d, jsobj *ev, const char *type)
{
    js_rt *rt = d->rt;
    jsval w = v_obj(rt->global);
    prop_put(rt, ev, atom_c(rt, "currentTarget"), w, 0);
    char pn[48];
    unsigned tl = (unsigned)strlen(type);
    if (tl <= 40) {
        pn[0] = 'o'; pn[1] = 'n'; memcpy(pn + 2, type, tl + 1);
        prop *p = prop_find(rt->global, atom_c(rt, pn));
        if (p && !(p->flags & PF_ACCESSOR) && is_callable(p->val)) { jsval a[1] = { v_obj(ev) }; call_safe(d, p->val, w, a, 1); }
    }
    for (int i = 0; i < d->nwls; i++) {
        if (strcmp(d->wls[i].type->s, type)) continue;
        jsval a[1] = { v_obj(ev) };
        call_safe(d, d->wls[i].fn, w, a, 1);
    }
}

/* Dispatches ev (already created) at target. Returns 1 if the default action was prevented. */
static int dispatch(dompage *d, dnode *target, jsobj *ev, const char *type, int bubbles)
{
    js_rt *rt = d->rt;
    if (d->depth >= 6) return 0;
    d->depth++;
    prop_put(rt, ev, atom_c(rt, "target"), target ? dn_wrap(d, target) : v_obj(rt->global), 0);
    int stopped = 0;
    if (target) {
        for (dnode *n = target; n && !stopped; n = n->parent) {
            fire_on(d, n, ev, type);
            if (ev_flag(rt, ev, "__stop")) stopped = 1;
            if (!bubbles) break;
        }
    }
    if (!stopped && (bubbles || !target)) fire_window(d, ev, type);
    d->depth--;
    return ev_flag(rt, ev, "defaultPrevented");
}

static jsval n_event_method(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_OBJ) return U;
    switch (rt->cur_magic) {
    case 0: prop_put(rt, thisv.u.o, atom_c(rt, "defaultPrevented"), v_bool(1), 0); break;
    case 1: prop_put(rt, thisv.u.o, atom_c(rt, "__stop"), v_bool(1), PF_HIDDEN); break;
    case 2: prop_put(rt, thisv.u.o, atom_c(rt, "__stop"), v_bool(1), PF_HIDDEN); prop_put(rt, thisv.u.o, atom_c(rt, "__stopnow"), v_bool(1), PF_HIDDEN); break;
    }
    return U;
}

static jsval n_Event(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    jstr *t = to_str(rt, ARG(0)); if (BAD()) return U;
    jsval init = ARG(1);
    int bubbles = 0;
    if (init.t == JT_OBJ) bubbles = to_bool(get_prop(rt, init, atom_c(rt, "bubbles")));
    jsobj *ev = make_event(d, t->s, bubbles);
    if (init.t == JT_OBJ) prop_put(rt, ev, atom_c(rt, "detail"), get_prop(rt, init, atom_c(rt, "detail")), 0);
    return v_obj(ev);
}

/* ---- style and classList ----------------------------------------------------------------------------------- */

static const struct { const char *js, *css; } css_props[] = {
    {"color", "color"}, {"backgroundColor", "background-color"}, {"background", "background"}, {"fontSize", "font-size"},
    {"fontWeight", "font-weight"}, {"fontStyle", "font-style"}, {"fontFamily", "font-family"}, {"textAlign", "text-align"},
    {"textDecoration", "text-decoration"}, {"display", "display"}, {"visibility", "visibility"}, {"width", "width"},
    {"height", "height"}, {"margin", "margin"}, {"marginTop", "margin-top"}, {"marginBottom", "margin-bottom"},
    {"marginLeft", "margin-left"}, {"marginRight", "margin-right"}, {"padding", "padding"}, {"paddingTop", "padding-top"},
    {"paddingBottom", "padding-bottom"}, {"paddingLeft", "padding-left"}, {"paddingRight", "padding-right"},
    {"border", "border"}, {"position", "position"}, {"top", "top"}, {"left", "left"}, {"right", "right"}, {"bottom", "bottom"},
    {"cursor", "cursor"}, {"opacity", "opacity"}, {"lineHeight", "line-height"}, {"cssFloat", "float"}, {"float", "float"},
    {"overflow", "overflow"}, {"zIndex", "z-index"}, {"textTransform", "text-transform"}, {"whiteSpace", "white-space"},
    {"listStyle", "list-style"}, {"verticalAlign", "vertical-align"}, {"maxWidth", "max-width"}, {"minWidth", "min-width"},
    {"maxHeight", "max-height"}, {"minHeight", "min-height"}, {"borderColor", "border-color"}, {"borderWidth", "border-width"},
    {"borderStyle", "border-style"}, {"fontVariant", "font-variant"}, {NULL, NULL}
};

/* the declarations of a style attribute, split into (name, value) pairs */
struct decl { char name[32], val[96]; };
static int style_parse(dnode *n, struct decl *out, int max)
{
    const char *raw = dn_attr_raw(n, "style");
    if (!raw) return 0;
    sbuf dec; sb_init(&dec);
    html_decode(raw, (unsigned)strlen(raw), &dec);
    const char *p = dec.p ? dec.p : "";
    int cnt = 0;
    while (*p && cnt < max) {
        while (*p == ' ' || *p == ';' || *p == '\n') p++;
        const char *ns = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *ne = p;
        while (ne > ns && (ne[-1] == ' ')) ne--;
        p++;
        while (*p == ' ') p++;
        const char *vs = p;
        while (*p && *p != ';') p++;
        const char *ve = p;
        while (ve > vs && ve[-1] == ' ') ve--;
        unsigned nl = (unsigned)(ne - ns), vl = (unsigned)(ve - vs);
        if (nl && nl < 32 && vl < 96) {
            for (unsigned i = 0; i < nl; i++) out[cnt].name[i] = (char)(ns[i] >= 'A' && ns[i] <= 'Z' ? ns[i] + 32 : ns[i]);
            out[cnt].name[nl] = 0;
            memcpy(out[cnt].val, vs, vl); out[cnt].val[vl] = 0;
            cnt++;
        }
    }
    sb_free(&dec);
    return cnt;
}

static void style_store(dompage *d, dnode *n, struct decl *ds, int cnt)
{
    sbuf b; sb_init(&b);
    for (int i = 0; i < cnt; i++) {
        sbuf one; sb_init(&one);
        sb_adds(&one, ds[i].name); sb_adds(&one, ": "); sb_adds(&one, ds[i].val); sb_addc(&one, ';');
        if (i) sb_addc(&b, ' ');
        html_encode(one.p, one.len, &b, 1);
        sb_free(&one);
    }
    dn_set_attr_raw(d, n, atom_c(d->rt, "style"), b.p ? b.p : "", b.len);
    sb_free(&b);
}

static void style_set(dompage *d, dnode *n, const char *name, const char *val)
{
    struct decl ds[40];
    int cnt = style_parse(n, ds, 39);
    int at = -1;
    for (int i = 0; i < cnt; i++) if (!strcmp(ds[i].name, name)) { at = i; break; }
    if (!val[0]) {                                           /* empty value removes the declaration */
        if (at < 0) return;
        for (int i = at; i + 1 < cnt; i++) ds[i] = ds[i + 1];
        cnt--;
    } else {
        if (at < 0) { if (cnt >= 39) return; at = cnt++; }
        strlcpy(ds[at].name, name, sizeof ds[at].name);
        strlcpy(ds[at].val, val, sizeof ds[at].val);
    }
    style_store(d, n, ds, cnt);
}

static jsval n_style_get(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    struct decl ds[40];
    int cnt = style_parse(n, ds, 40);
    const char *want = css_props[rt->cur_magic].css;
    for (int i = 0; i < cnt; i++) if (!strcmp(ds[i].name, want)) return vstr(rt, ds[i].val);
    return v_str(rt->a_empty);
}

static jsval n_style_set(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    int m = rt->cur_magic;
    jstr *v = to_str(rt, ARG(0)); if (BAD()) return U;
    style_set(D(rt), n, css_props[m].css, v->s);
    return U;
}

static jsval n_style_method(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    dompage *d = D(rt);
    int m = rt->cur_magic;
    jstr *name = to_str(rt, ARG(0)); if (BAD()) return U;
    if (m == 0) {                                           /* setProperty(name, value) */
        jstr *v = to_str(rt, ARG(1)); if (BAD()) return U;
        style_set(d, n, name->s, v->s);
        return U;
    }
    struct decl ds[40];
    int cnt = style_parse(n, ds, 40);
    for (int i = 0; i < cnt; i++) {
        if (strcmp(ds[i].name, name->s)) continue;
        if (m == 1) return vstr(rt, ds[i].val);             /* getPropertyValue */
        jsval old = vstr(rt, ds[i].val);                    /* removeProperty */
        style_set(d, n, name->s, "");
        return old;
    }
    return m == 1 ? v_str(rt->a_empty) : U;
}

static jsval n_style_cssText(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    if (rt->cur_magic == 0) {
        const char *raw = dn_attr_raw(n, "style");
        sbuf dec; sb_init(&dec);
        if (raw) html_decode(raw, (unsigned)strlen(raw), &dec);
        jstr *s = sb_str(rt, &dec);
        sb_free(&dec);
        return v_str(s);
    }
    jstr *v = to_str(rt, ARG(0)); if (BAD()) return U;
    sbuf enc; sb_init(&enc);
    html_encode(v->s, v->len, &enc, 1);
    dn_set_attr_raw(D(rt), n, atom_c(rt, "style"), enc.p ? enc.p : "", enc.len);
    sb_free(&enc);
    return U;
}

/* class lists */
static void class_words(dnode *n, sbuf *out_words)
{
    const char *raw = dn_attr_raw(n, "class");
    if (raw) sb_add(out_words, raw, (unsigned)strlen(raw));
}

static jsval n_classlist_method(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    dompage *d = D(rt);
    int m = rt->cur_magic;                                  /* 0 add, 1 remove, 2 toggle, 3 contains, 4 item */
    if (m == 4) {
        long long idx = num_to_int64(to_num(rt, ARG(0)));
        sbuf w; sb_init(&w); class_words(n, &w);
        const char *p = w.p ? w.p : ""; long long k = 0; jsval r = v_null();
        while (*p) {
            while (*p == ' ') p++;
            const char *s = p; while (*p && *p != ' ') p++;
            if (p > s && k++ == idx) { r = v_str(str_new(rt, s, (unsigned)(p - s))); break; }
        }
        sb_free(&w);
        return r;
    }
    int result = 0;
    for (int ai = 0; ai < nargs || (ai == 0 && nargs == 0); ai++) {
        if (ai >= nargs) break;
        jstr *cls = to_str(rt, args[ai]); if (BAD()) return U;
        if (!cls->len) continue;
        sbuf w; sb_init(&w); class_words(n, &w);
        sbuf out; sb_init(&out);
        const char *p = w.p ? w.p : "";
        int present = 0;
        while (*p) {
            while (*p == ' ') p++;
            const char *s = p; while (*p && *p != ' ') p++;
            if (p == s) break;
            if ((unsigned)(p - s) == cls->len && !memcmp(s, cls->s, cls->len)) { present = 1; if (m == 1 || (m == 2)) continue; }
            if (out.len) sb_addc(&out, ' ');
            sb_add(&out, s, (unsigned)(p - s));
        }
        int add = (m == 0) || (m == 2 && !present);
        if (m == 3) { result = present; sb_free(&w); sb_free(&out); break; }
        if (m == 2 && nargs > 1) { add = to_bool(args[1]); if (!add && !present) { result = 0; sb_free(&w); sb_free(&out); continue; } if (add && present) { result = 1; sb_free(&w); sb_free(&out); continue; } }
        if (add && !(m == 0 && present)) { if (out.len) sb_addc(&out, ' '); html_encode(cls->s, cls->len, &out, 1); }
        else if (add && present) { /* already there: keep as is */ }
        if (m == 2) result = add;
        dn_set_attr_raw(d, n, atom_c(rt, "class"), out.p ? out.p : "", out.len);
        sb_free(&w); sb_free(&out);
    }
    return (m == 2 || m == 3) ? v_bool(result) : U;
}

static jsval n_classlist_get(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    sbuf w; sb_init(&w); class_words(n, &w);
    if (rt->cur_magic == 1) {
        jstr *s = sb_str(rt, &w); sb_free(&w);
        return v_str(s);
    }
    long long k = 0; const char *p = w.p ? w.p : "";
    while (*p) { while (*p == ' ') p++; const char *s = p; while (*p && *p != ' ') p++; if (p > s) k++; }
    sb_free(&w);
    return vnum(k);
}

/* ---- node accessors ---------------------------------------------------------------------------------------- */

enum { G_NODETYPE, G_NODENAME, G_PARENT, G_PARENTEL, G_FIRST, G_LAST, G_NEXT, G_PREV, G_FIRSTEL, G_LASTEL, G_NEXTEL, G_PREVEL,
       G_CHILDNODES, G_CHILDREN, G_CHILDCOUNT, G_TEXT, G_INNER, G_OUTER, G_STYLE, G_CLASSLIST, G_VALUE, G_DATA, G_DATASET,
       G_OWNERDOC, G_ZERO, G_TABINDEX };

static const struct { const char *js, *attr; } reflect[] = {
    {"id", "id"}, {"className", "class"}, {"title", "title"}, {"href", "href"}, {"src", "src"}, {"type", "type"},
    {"name", "name"}, {"placeholder", "placeholder"}, {"alt", "alt"}, {"htmlFor", "for"}, {"action", "action"},
    {"method", "method"}, {"lang", "lang"}, {"rel", "rel"}, {"target", "target"}, {"width", "width"}, {"height", "height"},
    {"colSpan", "colspan"}, {"rowSpan", "rowspan"}, {"dir", "dir"}, {"maxLength", "maxlength"}, {"min", "min"}, {"max", "max"},
    {"step", "step"}, {"pattern", "pattern"}, {NULL, NULL}
};
static const struct { const char *js, *attr; } boolattr[] = {
    {"checked", "checked"}, {"disabled", "disabled"}, {"hidden", "hidden"}, {"selected", "selected"}, {"readOnly", "readonly"},
    {"required", "required"}, {"multiple", "multiple"}, {"autofocus", "autofocus"}, {NULL, NULL}
};
#define REFLECT_BASE 100
#define BOOL_BASE 200

static dnode *first_el(dnode *n) { for (dnode *k = n->first; k; k = k->next) if (k->type == DN_ELEMENT) return k; return NULL; }
static dnode *last_el(dnode *n) { for (dnode *k = n->last; k; k = k->prev) if (k->type == DN_ELEMENT) return k; return NULL; }
static dnode *next_el(dnode *n) { for (dnode *k = n->next; k; k = k->next) if (k->type == DN_ELEMENT) return k; return NULL; }
static dnode *prev_el(dnode *n) { for (dnode *k = n->prev; k; k = k->prev) if (k->type == DN_ELEMENT) return k; return NULL; }

static jsval child_object(js_rt *rt, dompage *d, dnode *n, const char *propname, jsobj *proto)
{
    jstr *key = atom_c(rt, propname);
    if (n->wrapper) { prop *p = prop_find(n->wrapper, key); if (p) return p->val; }
    jsobj *o = obj_new(rt, proto, CL_OBJECT);
    prop_put(rt, o, d->a_n, vnum(n->id), PF_HIDDEN);
    if (n->wrapper) prop_put(rt, n->wrapper, key, v_obj(o), PF_HIDDEN);
    return v_obj(o);
}

static jsval n_node_get(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    dompage *d = D(rt);
    int m = rt->cur_magic;
    if (m >= BOOL_BASE) return v_bool(dn_attr_raw(n, boolattr[m - BOOL_BASE].attr) != NULL);
    if (m >= REFLECT_BASE) {
        const char *raw = dn_attr_raw(n, reflect[m - REFLECT_BASE].attr);
        sbuf dec; sb_init(&dec);
        if (raw) html_decode(raw, (unsigned)strlen(raw), &dec);
        jstr *s = sb_str(rt, &dec); sb_free(&dec);
        return v_str(s);
    }
    switch (m) {
    case G_NODETYPE: return vnum(n->type);
    case G_NODENAME: case G_TABINDEX: {
        if (n->type == DN_ELEMENT) {
            char up[48]; unsigned i = 0;
            for (; i < n->tag->len && i < 47; i++) { char c = n->tag->s[i]; up[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
            up[i] = 0;
            return vstr(rt, up);
        }
        return v_str(n->tag);
    }
    case G_PARENT: return dn_wrap(d, n->parent);
    case G_PARENTEL: return dn_wrap(d, n->parent && n->parent->type == DN_ELEMENT ? n->parent : NULL);
    case G_FIRST: return dn_wrap(d, n->first);
    case G_LAST: return dn_wrap(d, n->last);
    case G_NEXT: return dn_wrap(d, n->next);
    case G_PREV: return dn_wrap(d, n->prev);
    case G_FIRSTEL: return dn_wrap(d, first_el(n));
    case G_LASTEL: return dn_wrap(d, last_el(n));
    case G_NEXTEL: return dn_wrap(d, next_el(n));
    case G_PREVEL: return dn_wrap(d, prev_el(n));
    case G_CHILDNODES: case G_CHILDREN: {
        jsobj *a = arr_new(rt, 0);
        for (dnode *k = n->first; k; k = k->next) if (m == G_CHILDNODES || k->type == DN_ELEMENT) arr_push(rt, a, dn_wrap(d, k));
        return v_obj(a);
    }
    case G_CHILDCOUNT: { int c = 0; for (dnode *k = n->first; k; k = k->next) if (k->type == DN_ELEMENT) c++; return vnum(c); }
    case G_TEXT: case G_DATA: {
        sbuf t; sb_init(&t);
        dn_text_content(n, &t);
        jstr *s = sb_str(rt, &t); sb_free(&t);
        return v_str(s);
    }
    case G_INNER: case G_OUTER: {
        sbuf t; sb_init(&t);
        dn_serialize(d, n, &t, m == G_INNER, 0);
        jstr *s = sb_str(rt, &t); sb_free(&t);
        return v_str(s);
    }
    case G_STYLE: return child_object(rt, d, n, "__style", d->proto_style);
    case G_CLASSLIST: return child_object(rt, d, n, "__classList", d->proto_classlist);
    case G_VALUE: {
        sbuf t; sb_init(&t);
        if (dn_is(n, "textarea")) dn_text_content(n, &t);
        else { const char *raw = dn_attr_raw(n, "value"); if (raw) html_decode(raw, (unsigned)strlen(raw), &t); }
        jstr *s = sb_str(rt, &t); sb_free(&t);
        return v_str(s);
    }
    case G_DATASET: {
        jsobj *o = obj_new(rt, rt->proto_object, CL_OBJECT);
        for (int i = 0; i < n->nattrs; i++) {
            const char *an = n->attrs[i].name->s;
            if (strncmp(an, "data-", 5) || !strcmp(an, "data-nid")) continue;
            char key[48]; unsigned k = 0;
            for (const char *p = an + 5; *p && k < 46; p++) {
                if (*p == '-' && p[1] >= 'a' && p[1] <= 'z') { key[k++] = (char)(p[1] - 32); p++; }
                else key[k++] = *p;
            }
            key[k] = 0;
            sbuf dec; sb_init(&dec);
            html_decode(n->attrs[i].val, (unsigned)strlen(n->attrs[i].val), &dec);
            prop_put(rt, o, atom_c(rt, key), v_str(sb_str(rt, &dec)), 0);
            sb_free(&dec);
        }
        return v_obj(o);
    }
    case G_OWNERDOC: return dn_wrap(d, d->doc);
    case G_ZERO: return vnum(0);
    }
    return U;
}

static jsval n_node_set(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    dompage *d = D(rt);
    int m = rt->cur_magic;
    jsval v = ARG(0);
    if (m >= BOOL_BASE) {
        jstr *an = atom_c(rt, boolattr[m - BOOL_BASE].attr);
        if (to_bool(v)) dn_set_attr_raw(d, n, an, "", 0); else dn_del_attr(d, n, an);
        return U;
    }
    if (m >= REFLECT_BASE) {
        jstr *s = to_str(rt, v); if (BAD()) return U;
        sbuf enc; sb_init(&enc);
        html_encode(s->s, s->len, &enc, 1);
        dn_set_attr_raw(d, n, atom_c(rt, reflect[m - REFLECT_BASE].attr), enc.p ? enc.p : "", enc.len);
        sb_free(&enc);
        return U;
    }
    jstr *s = (v.t == JT_NULL || v.t == JT_UNDEF) && (m == G_TEXT || m == G_INNER) ? rt->a_empty : to_str(rt, v);
    if (BAD()) return U;
    switch (m) {
    case G_TEXT: case G_DATA: {
        sbuf enc; sb_init(&enc);
        html_encode(s->s, s->len, &enc, 0);
        if (n->type == DN_TEXT) dn_set_text(d, n, enc.p ? enc.p : "", enc.len);
        else {
            dn_clear_children(d, n);
            if (enc.len) { dnode *t = dn_new(d, DN_TEXT, atom_c(rt, "#text")); dn_set_text(d, t, enc.p, enc.len); dn_append(d, n, t); }
        }
        sb_free(&enc);
        break;
    }
    case G_INNER:
        dn_clear_children(d, n);
        dn_parse(d, n, s->s, s->len);
        d->dirty = 1;
        break;
    case G_OUTER: {
        if (!n->parent) break;
        dnode *frag = dn_new(d, DN_FRAGMENT, atom_c(rt, "#fragment"));
        dn_parse(d, frag, s->s, s->len);
        dn_insert_before(d, n->parent, frag, n);
        dn_detach(d, n);
        break;
    }
    case G_VALUE: {
        sbuf enc; sb_init(&enc);
        html_encode(s->s, s->len, &enc, !dn_is(n, "textarea"));
        if (dn_is(n, "textarea")) {
            dn_clear_children(d, n);
            if (enc.len) { dnode *t = dn_new(d, DN_TEXT, atom_c(rt, "#text")); dn_set_text(d, t, enc.p, enc.len); dn_append(d, n, t); }
        } else dn_set_attr_raw(d, n, atom_c(rt, "value"), enc.p ? enc.p : "", enc.len);
        sb_free(&enc);
        break;
    }
    }
    return U;
}

/* ---- node methods ------------------------------------------------------------------------------------------ */

enum { M_GETATTR, M_SETATTR, M_REMATTR, M_HASATTR, M_APPEND, M_REMOVECHILD, M_INSERTBEFORE, M_REPLACECHILD, M_REMOVE,
       M_APPENDMANY, M_PREPEND, M_CLONE, M_CONTAINS, M_QS, M_QSA, M_BYTAG, M_BYCLASS, M_MATCHES, M_CLOSEST, M_ADDL, M_REML,
       M_DISPATCH, M_CLICK, M_NOOP, M_INSERTADJ, M_HASCHILD, M_RECT, M_ATTRNAMES, M_BEFORE, M_AFTER };

/* appends/prepends nodes or strings given as arguments */
static void add_nodes(js_rt *rt, dompage *d, dnode *parent, dnode *ref, jsval *args, int nargs)
{
    for (int i = 0; i < nargs; i++) {
        dnode *c = nd(rt, args[i]);
        if (!c) {
            jstr *s = to_str(rt, args[i]); if (BAD()) return;
            sbuf enc; sb_init(&enc);
            html_encode(s->s, s->len, &enc, 0);
            c = dn_new(d, DN_TEXT, atom_c(rt, "#text"));
            dn_set_text(d, c, enc.p ? enc.p : "", enc.len);
            sb_free(&enc);
        }
        dn_insert_before(d, parent, c, ref);
    }
}

static jsval n_node_method(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    THIS_NODE();
    dompage *d = D(rt);
    int m = rt->cur_magic;
    switch (m) {
    case M_GETATTR: {
        jstr *name = lower_atom(rt, to_str(rt, ARG(0))); if (BAD()) return U;
        const char *raw = dn_attr_raw(n, name->s);
        if (!raw) return v_null();
        sbuf dec; sb_init(&dec);
        html_decode(raw, (unsigned)strlen(raw), &dec);
        jstr *s = sb_str(rt, &dec); sb_free(&dec);
        return v_str(s);
    }
    case M_SETATTR: {
        jstr *name = lower_atom(rt, to_str(rt, ARG(0)));
        jstr *val = to_str(rt, ARG(1)); if (BAD()) return U;
        sbuf enc; sb_init(&enc);
        html_encode(val->s, val->len, &enc, 1);
        dn_set_attr_raw(d, n, name, enc.p ? enc.p : "", enc.len);
        sb_free(&enc);
        if (n->wrapper && name->len > 2 && name->s[0] == 'o' && name->s[1] == 'n') prop_del(n->wrapper, name);   /* recompile the handler */
        return U;
    }
    case M_REMATTR: { jstr *name = lower_atom(rt, to_str(rt, ARG(0))); if (BAD()) return U; dn_del_attr(d, n, name); return U; }
    case M_HASATTR: { jstr *name = lower_atom(rt, to_str(rt, ARG(0))); if (BAD()) return U; return v_bool(dn_attr_raw(n, name->s) != NULL); }
    case M_ATTRNAMES: {
        jsobj *a = arr_new(rt, 0);
        for (int i = 0; i < n->nattrs; i++) arr_push(rt, a, v_str(n->attrs[i].name));
        return v_obj(a);
    }
    case M_APPEND: { dnode *c = arg_node(rt, ARG(0)); if (!c) return U; dn_append(d, n, c); return ARG(0); }
    case M_REMOVECHILD: {
        dnode *c = arg_node(rt, ARG(0)); if (!c) return U;
        if (c->parent != n) return throw_error(rt, "Error", "The node to be removed is not a child of this node", NULL, NULL);
        dn_detach(d, c);
        return ARG(0);
    }
    case M_INSERTBEFORE: {
        dnode *c = arg_node(rt, ARG(0)); if (!c) return U;
        dnode *ref = ARG(1).t == JT_NULL || ARG(1).t == JT_UNDEF ? NULL : arg_node(rt, ARG(1));
        if (BAD()) return U;
        dn_insert_before(d, n, c, ref);
        return ARG(0);
    }
    case M_REPLACECHILD: {
        dnode *c = arg_node(rt, ARG(0)), *old = arg_node(rt, ARG(1));
        if (!c || !old) return U;
        dn_insert_before(d, n, c, old);
        dn_detach(d, old);
        return ARG(1);
    }
    case M_REMOVE: dn_detach(d, n); return U;
    case M_APPENDMANY: add_nodes(rt, d, n, NULL, args, nargs); return U;
    case M_PREPEND: add_nodes(rt, d, n, n->first, args, nargs); return U;
    case M_BEFORE: if (n->parent) add_nodes(rt, d, n->parent, n, args, nargs); return U;
    case M_AFTER: if (n->parent) add_nodes(rt, d, n->parent, n->next, args, nargs); return U;
    case M_CLONE: return dn_wrap(d, dn_clone(d, n, to_bool(ARG(0))));
    case M_CONTAINS: { dnode *c = nd(rt, ARG(0)); for (; c; c = c->parent) if (c == n) return v_bool(1); return v_bool(0); }
    case M_QS: case M_QSA: {
        jstr *sel = to_str(rt, ARG(0)); if (BAD()) return U;
        if (m == M_QSA) { jsobj *out = arr_new(rt, 0); sel_query(d, n, sel->s, 1, out, NULL); return v_obj(out); }
        dnode *f = NULL;
        sel_query(d, n, sel->s, 0, NULL, &f);
        return dn_wrap(d, f);
    }
    case M_BYTAG: { jstr *t = lower_atom(rt, to_str(rt, ARG(0))); if (BAD()) return U; jsobj *out = arr_new(rt, 0); dn_by_tag(d, n, t->s, out); return v_obj(out); }
    case M_BYCLASS: { jstr *c = to_str(rt, ARG(0)); if (BAD()) return U; jsobj *out = arr_new(rt, 0); dn_by_class(d, n, c->s, out); return v_obj(out); }
    case M_MATCHES: { jstr *sel = to_str(rt, ARG(0)); if (BAD()) return U; return v_bool(sel_matches(d, n, sel->s)); }
    case M_CLOSEST: {
        jstr *sel = to_str(rt, ARG(0)); if (BAD()) return U;
        for (dnode *k = n; k; k = k->parent) if (k->type == DN_ELEMENT && sel_matches(d, k, sel->s)) return dn_wrap(d, k);
        return v_null();
    }
    case M_ADDL: case M_REML: {
        jstr *type = to_str(rt, ARG(0)); if (BAD()) return U;
        jsval fn = ARG(1);
        if (!is_callable(fn)) return U;
        jstr *ta = to_atom(rt, type);
        if (m == M_REML) {
            for (int i = 0; i < n->nls; i++)
                if (n->ls[i].type == ta && n->ls[i].fn.u.o == fn.u.o) { memmove(&n->ls[i], &n->ls[i + 1], (size_t)(n->nls - i - 1) * sizeof(dlistener)); n->nls--; d->dirty = 1; break; }
            return U;
        }
        for (int i = 0; i < n->nls; i++) if (n->ls[i].type == ta && n->ls[i].fn.u.o == fn.u.o) return U;
        if (n->nls == n->lcap) {
            int nc = n->lcap ? n->lcap * 2 : 2;
            dlistener *nl = (dlistener *)JS_REALLOC(n->ls, (size_t)nc * sizeof(dlistener));
            if (!nl) { rt->abort_msg = "out of memory"; return U; }
            n->ls = nl; n->lcap = nc;
        }
        n->ls[n->nls].type = ta; n->ls[n->nls].fn = fn; n->nls++;
        arr_push(rt, d->roots, fn);
        if (!strcmp(ta->s, "click")) d->dirty = 1;           /* the element becomes clickable: render it again */
        return U;
    }
    case M_DISPATCH: {
        jsval ev = ARG(0);
        if (ev.t != JT_OBJ) return throw_error(rt, "TypeError", "dispatchEvent needs an Event", NULL, NULL);
        jstr *type = to_str(rt, get_prop(rt, ev, atom_c(rt, "type"))); if (BAD()) return U;
        int bubbles = to_bool(get_prop(rt, ev, atom_c(rt, "bubbles")));
        int prevented = dispatch(d, n, ev.u.o, type->s, bubbles);
        return v_bool(!prevented);
    }
    case M_CLICK: {
        jsobj *ev = make_event(d, "click", 1);
        dispatch(d, n, ev, "click", 1);
        return U;
    }
    case M_INSERTADJ: {
        jstr *pos = lower_atom(rt, to_str(rt, ARG(0)));
        jstr *html = to_str(rt, ARG(1)); if (BAD()) return U;
        dnode *frag = dn_new(d, DN_FRAGMENT, atom_c(rt, "#fragment"));
        dn_parse(d, frag, html->s, html->len);
        if (!strcmp(pos->s, "beforeend")) dn_append(d, n, frag);
        else if (!strcmp(pos->s, "afterbegin")) dn_insert_before(d, n, frag, n->first);
        else if (!strcmp(pos->s, "beforebegin") && n->parent) dn_insert_before(d, n->parent, frag, n);
        else if (!strcmp(pos->s, "afterend") && n->parent) dn_insert_before(d, n->parent, frag, n->next);
        return U;
    }
    case M_HASCHILD: return v_bool(n->first != NULL);
    case M_RECT: {
        jsobj *r = obj_new(rt, rt->proto_object, CL_OBJECT);
        static const char *k[] = { "x", "y", "top", "left", "right", "bottom", "width", "height" };
        for (int i = 0; i < 8; i++) prop_put(rt, r, atom_c(rt, k[i]), vnum(0), 0);
        return v_obj(r);
    }
    }
    return U;
}

/* ---- document ---------------------------------------------------------------------------------------------- */

enum { D_BYID, D_CREATEEL, D_CREATETEXT, D_CREATEFRAG, D_WRITE, D_WRITELN };

static dnode *find_by_tag(dnode *n, const char *tag)
{
    for (dnode *k = n->first; k; k = k->next) {
        if (k->type != DN_ELEMENT) continue;
        if (!strcmp(k->tag->s, tag)) return k;
        dnode *r = find_by_tag(k, tag);
        if (r) return r;
    }
    return NULL;
}

static jsval n_document_method(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    switch (rt->cur_magic) {
    case D_BYID: { jstr *id = to_str(rt, ARG(0)); if (BAD()) return U; return dn_wrap(d, dn_by_id(d, d->doc, id->s)); }
    case D_CREATEEL: { jstr *t = lower_atom(rt, to_str(rt, ARG(0))); if (BAD()) return U; return dn_wrap(d, dn_new(d, DN_ELEMENT, t)); }
    case D_CREATETEXT: {
        jstr *s = to_str(rt, ARG(0)); if (BAD()) return U;
        sbuf enc; sb_init(&enc);
        html_encode(s->s, s->len, &enc, 0);
        dnode *t = dn_new(d, DN_TEXT, atom_c(rt, "#text"));
        dn_set_text(d, t, enc.p ? enc.p : "", enc.len);
        sb_free(&enc);
        return dn_wrap(d, t);
    }
    case D_CREATEFRAG: return dn_wrap(d, dn_new(d, DN_FRAGMENT, atom_c(rt, "#fragment")));
    case D_WRITE: case D_WRITELN: {
        sbuf all; sb_init(&all);
        for (int i = 0; i < nargs; i++) { jstr *s = to_str(rt, args[i]); if (BAD()) { sb_free(&all); return U; } sb_add(&all, s->s, s->len); }
        if (rt->cur_magic == D_WRITELN) sb_addc(&all, '\n');
        dnode *frag = dn_new(d, DN_FRAGMENT, atom_c(rt, "#fragment"));
        dn_parse(d, frag, all.p ? all.p : "", all.len);
        sb_free(&all);
        if (d->cur_script && d->cur_script->parent) dn_insert_before(d, d->cur_script->parent, frag, d->cur_script->next);
        else if (d->body) dn_append(d, d->body, frag);
        return U;
    }
    }
    return U;
}

enum { DG_BODY, DG_HEAD, DG_ROOT, DG_TITLE, DG_COOKIE, DG_READY, DG_URL, DG_REFERRER, DG_WINDOW, DG_LOCATION, DG_NULL, DG_CHARSET };

static jsval n_document_get(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    switch (rt->cur_magic) {
    case DG_BODY: return dn_wrap(d, d->body);
    case DG_HEAD: return dn_wrap(d, d->head);
    case DG_ROOT: return dn_wrap(d, d->html);
    case DG_TITLE: {
        dnode *t = find_by_tag(d->doc, "title");
        sbuf s; sb_init(&s);
        if (t) dn_text_content(t, &s);
        jstr *r = sb_str(rt, &s); sb_free(&s);
        return v_str(r);
    }
    case DG_COOKIE: return v_str(rt->a_empty);
    case DG_READY: return vstr(rt, d->ready == 0 ? "loading" : d->ready == 1 ? "interactive" : "complete");
    case DG_URL: return vstr(rt, d->base);
    case DG_REFERRER: return v_str(rt->a_empty);
    case DG_WINDOW: return v_obj(rt->global);
    case DG_LOCATION: return v_obj(d->location);
    case DG_CHARSET: return vstr(rt, "UTF-8");
    }
    return v_null();
}

static jsval n_document_set(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    jsval v = ARG(0);
    if (rt->cur_magic == DG_TITLE) {
        jstr *s = to_str(rt, v); if (BAD()) return U;
        dnode *t = find_by_tag(d->doc, "title");
        if (!t && d->head) { t = dn_new(d, DN_ELEMENT, atom_c(rt, "title")); dn_append(d, d->head, t); }
        if (t) {
            sbuf enc; sb_init(&enc);
            html_encode(s->s, s->len, &enc, 0);
            dn_clear_children(d, t);
            dnode *tx = dn_new(d, DN_TEXT, atom_c(rt, "#text"));
            dn_set_text(d, tx, enc.p ? enc.p : "", enc.len);
            dn_append(d, t, tx);
            sb_free(&enc);
        }
    } else if (rt->cur_magic == DG_LOCATION) {
        jstr *u = to_str(rt, v); if (BAD()) return U;
        if (d->host.navigate) d->host.navigate(d->host.ud, u->s);
    }
    return U;
}

/* ---- location ---------------------------------------------------------------------------------------------- */

enum { L_HREF, L_HOST, L_HOSTNAME, L_PATHNAME, L_SEARCH, L_HASH, L_PROTOCOL, L_ORIGIN };

static jsval n_location_get(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    const char *u = d->base;
    int m = rt->cur_magic;
    if (m == L_HREF) return vstr(rt, u);
    const char *p = u, *sch_end = strstr(u, "://");
    const char *host = sch_end ? sch_end + 3 : u, *he = host;
    while (*he && *he != '/' && *he != '?' && *he != '#') he++;
    const char *path = he, *pe = path;
    while (*pe && *pe != '?' && *pe != '#') pe++;
    const char *q = pe, *qe = q;
    if (*q == '?') while (*qe && *qe != '#') qe++; else qe = q;
    const char *h = qe;
    sbuf b; sb_init(&b);
    switch (m) {
    case L_PROTOCOL: sb_add(&b, p, sch_end ? (unsigned)(sch_end - p) + 1 : 0); break;
    case L_HOST: sb_add(&b, host, (unsigned)(he - host)); break;
    case L_HOSTNAME: { const char *c = host; while (c < he && *c != ':') c++; sb_add(&b, host, (unsigned)(c - host)); break; }
    case L_ORIGIN: sb_add(&b, p, (unsigned)(he - p)); break;
    case L_PATHNAME: if (pe == path) sb_addc(&b, '/'); else sb_add(&b, path, (unsigned)(pe - path)); break;
    case L_SEARCH: sb_add(&b, q, (unsigned)(qe - q)); break;
    case L_HASH: sb_adds(&b, *h == '#' && h[1] ? h : ""); break;
    }
    jstr *s = sb_str(rt, &b); sb_free(&b);
    return v_str(s);
}

static jsval n_location_set(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    jstr *u = to_str(rt, ARG(0)); if (BAD()) return U;
    if (d->host.navigate) d->host.navigate(d->host.ud, u->s);
    return U;
}

static jsval n_location_method(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    if (rt->cur_magic == 1) { if (d->host.navigate) d->host.navigate(d->host.ud, d->base); return U; }
    jstr *u = to_str(rt, ARG(0)); if (BAD()) return U;
    if (d->host.navigate) d->host.navigate(d->host.ud, u->s);
    return U;
}

/* ---- window: alerts, timers -------------------------------------------------------------------------------- */

static jsval n_alert(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    int m = rt->cur_magic;                                   /* 0 alert, 1 confirm, 2 prompt */
    jstr *s = to_str(rt, ARG(0)); if (BAD()) return U;
    sbuf b; sb_init(&b);
    sb_adds(&b, m == 0 ? "alert: " : m == 1 ? "confirm: " : "prompt: ");
    sb_add(&b, s->s, s->len);
    host_log(d, b.p, b.len);
    sb_free(&b);
    if (m == 1) return v_bool(1);
    if (m == 2) return ARG(1).t == JT_UNDEF ? v_null() : ARG(1);
    return U;
}

static int timer_slot(dompage *d)
{
    for (int i = 0; i < d->ntimers; i++) if (!d->timers[i].active) return i;
    if (d->ntimers == d->tcap) {
        int nc = d->tcap ? d->tcap * 2 : 8;
        struct dtimer *nt = (struct dtimer *)JS_REALLOC(d->timers, (size_t)nc * sizeof *nt);
        if (!nt) { d->rt->abort_msg = "out of memory"; return -1; }
        d->timers = nt; d->tcap = nc;
    }
    d->timers[d->ntimers].active = 0;
    arr_push(d->rt, d->timer_fns, v_undef());
    return d->ntimers++;
}

static jsval n_set_timer(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    int m = rt->cur_magic;                                   /* 0 setTimeout, 1 setInterval, 2 requestAnimationFrame */
    jsval fn = ARG(0);
    if (fn.t == JT_STR) fn = compile_handler(d, fn.u.s->s, fn.u.s->len);
    if (!is_callable(fn)) return throw_error(rt, "TypeError", "The callback provided is not a function", NULL, NULL);
    long long delay = m == 2 ? 16 : (ARG(1).t == JT_UNDEF ? 0 : num_to_int64(to_num(rt, ARG(1))));
    if (delay < 0) delay = 0;
    if (m == 1 && delay < 10) delay = 10;
    int slot = timer_slot(d);
    if (slot < 0) return U;
    struct dtimer *t = &d->timers[slot];
    t->id = ++d->next_timer_id;
    t->active = 1;
    t->due = (d->host.now_ms ? d->host.now_ms(d->host.ud) : 0) + delay;
    t->interval = m == 1 ? delay : 0;
    d->timer_fns->items[slot] = fn;
    return vnum(t->id);
}

static jsval n_clear_timer(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    long long id = num_to_int64(to_num(rt, ARG(0)));
    for (int i = 0; i < d->ntimers; i++)
        if (d->timers[i].active && d->timers[i].id == id) { d->timers[i].active = 0; d->timer_fns->items[i] = v_undef(); }
    return U;
}

static jsval n_window_listener(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    jstr *type = to_str(rt, ARG(0)); if (BAD()) return U;
    jsval fn = ARG(1);
    if (!is_callable(fn)) return U;
    jstr *ta = to_atom(rt, type);
    if (rt->cur_magic == 1) {
        for (int i = 0; i < d->nwls; i++)
            if (d->wls[i].type == ta && d->wls[i].fn.u.o == fn.u.o) { memmove(&d->wls[i], &d->wls[i + 1], (size_t)(d->nwls - i - 1) * sizeof(dlistener)); d->nwls--; break; }
        return U;
    }
    if (d->nwls == d->wcap) {
        int nc = d->wcap ? d->wcap * 2 : 4;
        dlistener *nl = (dlistener *)JS_REALLOC(d->wls, (size_t)nc * sizeof(dlistener));
        if (!nl) { rt->abort_msg = "out of memory"; return U; }
        d->wls = nl; d->wcap = nc;
    }
    d->wls[d->nwls].type = ta; d->wls[d->nwls].fn = fn; d->nwls++;
    arr_push(rt, d->roots, fn);
    return U;
}

static jsval n_get_computed_style(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dnode *n = nd(rt, ARG(0));
    if (!n) return v_obj(obj_new(rt, rt->proto_object, CL_OBJECT));
    return child_object(rt, D(rt), n, "__style", D(rt)->proto_style);
}

static jsval n_now(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    dompage *d = D(rt);
    return vnum(d->host.now_ms ? d->host.now_ms(d->host.ud) : 0);
}

static jsval n_noop(js_rt *rt, jsval thisv, jsval *args, int nargs) { return U; }

/* ---- setup ------------------------------------------------------------------------------------------------- */

static jsobj *proto_document_of(dompage *d)
{
    prop *p = prop_find(d->rt->global, atom_c(d->rt, "__proto_document"));
    return p && p->val.t == JT_OBJ ? p->val.u.o : d->proto_node;
}

static const char *prelude =
    "(function(){"
    "function Store(){ var d={}; return { getItem:function(k){ return k in d ? d[k] : null }, setItem:function(k,v){ d[k]=String(v) },"
    " removeItem:function(k){ delete d[k] }, clear:function(){ d={} }, key:function(i){ var k=Object.keys(d); return i<k.length?k[i]:null },"
    " get length(){ return Object.keys(d).length } } }"
    "window.localStorage=Store(); window.sessionStorage=Store();"
    "window.history={ pushState:function(){}, replaceState:function(){}, back:function(){}, forward:function(){}, go:function(){}, length:1, state:null };"
    "window.performance={ now:function(){ return Date.now() }, timing:{}, mark:function(){}, measure:function(){} };"
    "window.matchMedia=function(q){ return { matches:false, media:q, addListener:function(){}, removeListener:function(){}, addEventListener:function(){}, removeEventListener:function(){} } };"
    "window.screen={ width:800, height:600, availWidth:800, availHeight:600, colorDepth:24 };"
    "window.navigator={ userAgent:'Mozilla/5.0 (NimbusOS) NimbusBrowser', language:'en-US', languages:['en-US'], platform:'NimbusOS', onLine:true, cookieEnabled:false };"
    "window.innerWidth=760; window.innerHeight=480; window.outerWidth=760; window.outerHeight=520; window.devicePixelRatio=1;"
    "window.pageXOffset=0; window.pageYOffset=0; window.scrollX=0; window.scrollY=0;"
    "window.scrollTo=function(){}; window.scrollBy=function(){}; window.focus=function(){}; window.blur=function(){}; window.open=function(){ return null };"
    "window.close=function(){}; window.print=function(){}; window.getSelection=function(){ return { toString:function(){ return '' } } };"
    "window.top=window; window.parent=window; window.self=window; window.frames=window; window.name='';"
    "window.CustomEvent=Event; window.Element=Node; window.HTMLElement=Node; window.Document=Node; window.Text=Node;"
    "})();";

static jsobj *mk_native_ctor(js_rt *rt, const char *name, js_native fn, jsobj *proto)
{
    jsobj *c = native_new(rt, name, fn, 1, 0);
    prop_put(rt, c, rt->a_prototype, v_obj(proto), PF_HIDDEN);
    prop_put(rt, proto, rt->a_constructor, v_obj(c), PF_HIDDEN);
    prop_put(rt, rt->global, atom_c(rt, name), v_obj(c), PF_HIDDEN);
    return c;
}

static jsval n_Node_ctor(js_rt *rt, jsval thisv, jsval *args, int nargs) { return throw_error(rt, "TypeError", "Illegal constructor", NULL, NULL); }

static long long clock_adapter(void *ud)
{
    dompage *d = (dompage *)ud;
    return d->host.now_ms ? d->host.now_ms(d->host.ud) : 0;
}

static void console_out(void *ud, const char *s, int len)
{
    dompage *d = (dompage *)ud;
    while (len > 0 && s[len - 1] == '\n') len--;
    host_log(d, s, (unsigned)len);
}

static void setup_globals(dompage *d)
{
    js_rt *rt = d->rt;
    jsobj *g = rt->global;
    d->a_n = atom_c(rt, "__n");
    d->proto_node = obj_new(rt, rt->proto_object, CL_OBJECT);
    jsobj *pdoc = obj_new(rt, d->proto_node, CL_OBJECT);
    d->proto_style = obj_new(rt, rt->proto_object, CL_OBJECT);
    d->proto_classlist = obj_new(rt, rt->proto_object, CL_OBJECT);
    d->proto_event = obj_new(rt, rt->proto_object, CL_OBJECT);
    d->roots = arr_new(rt, 0);
    d->timer_fns = arr_new(rt, 0);
    js_pin(rt, v_obj(d->proto_node)); js_pin(rt, v_obj(pdoc)); js_pin(rt, v_obj(d->proto_style));
    js_pin(rt, v_obj(d->proto_classlist)); js_pin(rt, v_obj(d->proto_event)); js_pin(rt, v_obj(d->roots)); js_pin(rt, v_obj(d->timer_fns));
    prop_put(rt, g, atom_c(rt, "__proto_document"), v_obj(pdoc), PF_HIDDEN);

    /* Node / Element */
    jsobj *np = d->proto_node;
    static const struct { const char *n; int m; } getters[] = {
        {"nodeType", G_NODETYPE}, {"nodeName", G_NODENAME}, {"tagName", G_NODENAME}, {"parentNode", G_PARENT}, {"parentElement", G_PARENTEL},
        {"firstChild", G_FIRST}, {"lastChild", G_LAST}, {"nextSibling", G_NEXT}, {"previousSibling", G_PREV},
        {"firstElementChild", G_FIRSTEL}, {"lastElementChild", G_LASTEL}, {"nextElementSibling", G_NEXTEL}, {"previousElementSibling", G_PREVEL},
        {"childNodes", G_CHILDNODES}, {"children", G_CHILDREN}, {"childElementCount", G_CHILDCOUNT}, {"ownerDocument", G_OWNERDOC},
        {"offsetWidth", G_ZERO}, {"offsetHeight", G_ZERO}, {"clientWidth", G_ZERO}, {"clientHeight", G_ZERO}, {"scrollTop", G_ZERO},
        {"scrollLeft", G_ZERO}, {"scrollHeight", G_ZERO}, {"scrollWidth", G_ZERO}, {"offsetTop", G_ZERO}, {"offsetLeft", G_ZERO},
        {"classList", G_CLASSLIST}, {"style", G_STYLE}, {"dataset", G_DATASET}, {"outerHTML", G_OUTER}, {NULL, 0}
    };
    for (int i = 0; getters[i].n; i++) def_accessor(rt, np, getters[i].n, n_node_get, getters[i].m == G_OUTER ? n_node_set : NULL, getters[i].m);
    def_accessor(rt, np, "textContent", n_node_get, n_node_set, G_TEXT);
    def_accessor(rt, np, "innerText", n_node_get, n_node_set, G_TEXT);
    def_accessor(rt, np, "innerHTML", n_node_get, n_node_set, G_INNER);
    def_accessor(rt, np, "value", n_node_get, n_node_set, G_VALUE);
    def_accessor(rt, np, "nodeValue", n_node_get, n_node_set, G_DATA);
    def_accessor(rt, np, "data", n_node_get, n_node_set, G_DATA);
    for (int i = 0; reflect[i].js; i++) def_accessor(rt, np, reflect[i].js, n_node_get, n_node_set, REFLECT_BASE + i);
    for (int i = 0; boolattr[i].js; i++) def_accessor(rt, np, boolattr[i].js, n_node_get, n_node_set, BOOL_BASE + i);
    static const struct { const char *n; int m; } methods[] = {
        {"getAttribute", M_GETATTR}, {"setAttribute", M_SETATTR}, {"removeAttribute", M_REMATTR}, {"hasAttribute", M_HASATTR},
        {"getAttributeNames", M_ATTRNAMES}, {"appendChild", M_APPEND}, {"removeChild", M_REMOVECHILD}, {"insertBefore", M_INSERTBEFORE},
        {"replaceChild", M_REPLACECHILD}, {"remove", M_REMOVE}, {"append", M_APPENDMANY}, {"prepend", M_PREPEND}, {"before", M_BEFORE},
        {"after", M_AFTER}, {"cloneNode", M_CLONE}, {"contains", M_CONTAINS}, {"querySelector", M_QS}, {"querySelectorAll", M_QSA},
        {"getElementsByTagName", M_BYTAG}, {"getElementsByClassName", M_BYCLASS}, {"matches", M_MATCHES}, {"closest", M_CLOSEST},
        {"addEventListener", M_ADDL}, {"removeEventListener", M_REML}, {"dispatchEvent", M_DISPATCH}, {"click", M_CLICK},
        {"focus", M_NOOP}, {"blur", M_NOOP}, {"scrollIntoView", M_NOOP}, {"select", M_NOOP}, {"insertAdjacentHTML", M_INSERTADJ},
        {"hasChildNodes", M_HASCHILD}, {"getBoundingClientRect", M_RECT}, {"normalize", M_NOOP}, {NULL, 0}
    };
    for (int i = 0; methods[i].n; i++) def_native(rt, np, methods[i].n, n_node_method, 1, methods[i].m);

    /* style, classList, events */
    for (int i = 0; css_props[i].js; i++) def_accessor(rt, d->proto_style, css_props[i].js, n_style_get, n_style_set, i);
    def_accessor(rt, d->proto_style, "cssText", n_style_cssText, n_style_cssText, 0);
    def_native(rt, d->proto_style, "setProperty", n_style_method, 2, 0);
    def_native(rt, d->proto_style, "getPropertyValue", n_style_method, 1, 1);
    def_native(rt, d->proto_style, "removeProperty", n_style_method, 1, 2);
    static const char *clm[] = { "add", "remove", "toggle", "contains", "item" };
    for (int i = 0; i < 5; i++) def_native(rt, d->proto_classlist, clm[i], n_classlist_method, 1, i);
    def_accessor(rt, d->proto_classlist, "length", n_classlist_get, NULL, 0);
    def_accessor(rt, d->proto_classlist, "value", n_classlist_get, NULL, 1);
    def_native(rt, d->proto_event, "preventDefault", n_event_method, 0, 0);
    def_native(rt, d->proto_event, "stopPropagation", n_event_method, 0, 1);
    def_native(rt, d->proto_event, "stopImmediatePropagation", n_event_method, 0, 2);
    mk_native_ctor(rt, "Event", n_Event, d->proto_event);
    mk_native_ctor(rt, "Node", n_Node_ctor, np);

    /* document */
    static const struct { const char *n; int m; } dmeth[] = {
        {"getElementById", D_BYID}, {"createElement", D_CREATEEL}, {"createTextNode", D_CREATETEXT},
        {"createDocumentFragment", D_CREATEFRAG}, {"write", D_WRITE}, {"writeln", D_WRITELN}, {NULL, 0}
    };
    for (int i = 0; dmeth[i].n; i++) def_native(rt, pdoc, dmeth[i].n, n_document_method, 1, dmeth[i].m);
    static const struct { const char *n; int m, set; } dget[] = {
        {"body", DG_BODY, 0}, {"head", DG_HEAD, 0}, {"documentElement", DG_ROOT, 0}, {"title", DG_TITLE, 1}, {"cookie", DG_COOKIE, 1},
        {"readyState", DG_READY, 0}, {"URL", DG_URL, 0}, {"documentURI", DG_URL, 0}, {"referrer", DG_REFERRER, 0}, {"defaultView", DG_WINDOW, 0},
        {"location", DG_LOCATION, 1}, {"characterSet", DG_CHARSET, 0}, {"activeElement", DG_NULL, 0}, {NULL, 0, 0}
    };
    for (int i = 0; dget[i].n; i++) def_accessor(rt, pdoc, dget[i].n, n_document_get, dget[i].set ? n_document_set : NULL, dget[i].m);
    prop_put(rt, g, atom_c(rt, "document"), dn_wrap(d, d->doc), PF_HIDDEN);
    d->document = d->doc->wrapper;

    /* location */
    d->location = obj_new(rt, rt->proto_object, CL_OBJECT);
    js_pin(rt, v_obj(d->location));
    static const char *lnames[] = { "href", "host", "hostname", "pathname", "search", "hash", "protocol", "origin" };
    for (int i = 0; i < 8; i++) def_accessor(rt, d->location, lnames[i], n_location_get, i == 0 ? n_location_set : NULL, i);
    def_native(rt, d->location, "assign", n_location_method, 1, 0);
    def_native(rt, d->location, "replace", n_location_method, 1, 2);
    def_native(rt, d->location, "reload", n_location_method, 0, 1);
    def_native(rt, d->location, "toString", n_location_get, 0, L_HREF);
    def_accessor(rt, g, "location", n_document_get, n_document_set, DG_LOCATION);

    /* window */
    prop_put(rt, g, atom_c(rt, "window"), v_obj(g), PF_HIDDEN);
    def_native(rt, g, "alert", n_alert, 1, 0);
    def_native(rt, g, "confirm", n_alert, 1, 1);
    def_native(rt, g, "prompt", n_alert, 2, 2);
    def_native(rt, g, "setTimeout", n_set_timer, 2, 0);
    def_native(rt, g, "setInterval", n_set_timer, 2, 1);
    def_native(rt, g, "requestAnimationFrame", n_set_timer, 1, 2);
    def_native(rt, g, "clearTimeout", n_clear_timer, 1, 0);
    def_native(rt, g, "clearInterval", n_clear_timer, 1, 0);
    def_native(rt, g, "cancelAnimationFrame", n_clear_timer, 1, 0);
    def_native(rt, g, "addEventListener", n_window_listener, 2, 0);
    def_native(rt, g, "removeEventListener", n_window_listener, 2, 1);
    def_native(rt, g, "getComputedStyle", n_get_computed_style, 1, 0);
    def_native(rt, g, "queueMicrotask", n_set_timer, 1, 0);
    (void)n_now; (void)n_noop;
    char err[200];
    if (js_eval(rt, prelude, (int)strlen(prelude), err, sizeof err)) host_log(d, err, (unsigned)strlen(err));
}

/* ---- the page API ------------------------------------------------------------------------------------------ */

static dnode *find_el(dnode *n, const char *tag) { return find_by_tag(n, tag); }

dompage *dom_new(const char *html, const char *base_url, const struct dom_host *host)
{
    dompage *d = (dompage *)JS_MALLOC(sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    if (host) d->host = *host;
    strlcpy(d->base, base_url ? base_url : "", sizeof d->base);
    d->rt = js_new(console_out, d);
    if (!d->rt) { JS_FREE(d); return NULL; }
    js_rt *rt = d->rt;
    rt->dom = d;
#ifdef NIMBUS_KERNEL
    js_set_limits(rt, 16ul * 1024 * 1024, 8000000ul, 48);     /* the browser's stack is 256 KB: a call level costs about 1.4 KB */
#else
    js_set_limits(rt, 24ul * 1024 * 1024, 30000000ul, 0);
#endif
    js_set_clock(rt, clock_adapter, d);
    d->doc = dn_new(d, DN_DOCUMENT, atom_c(rt, "#document"));
    dn_parse(d, d->doc, html, (unsigned)strlen(html));
    d->html = find_el(d->doc, "html");
    d->head = find_el(d->doc, "head");
    d->body = find_el(d->doc, "body");
    if (!d->html) {                                          /* a fragment: wrap everything in <html><body> */
        d->html = dn_new(d, DN_ELEMENT, atom_c(rt, "html"));
        while (d->doc->first) { dnode *c = d->doc->first; dn_detach(d, c); dn_append(d, d->html, c); }
        dn_append(d, d->doc, d->html);
    }
    if (!d->head) { d->head = dn_new(d, DN_ELEMENT, atom_c(rt, "head")); dn_insert_before(d, d->html, d->head, d->html->first); }
    if (!d->body) {
        d->body = dn_new(d, DN_ELEMENT, atom_c(rt, "body"));
        dnode *k = d->html->first;
        while (k) { dnode *nx = k->next; if (k != d->head) { dn_detach(d, k); dn_append(d, d->body, k); } k = nx; }
        dn_append(d, d->html, d->body);
    }
    d->dirty = 0;
    setup_globals(d);
    d->dirty = 0;
    return d;
}

void dom_free(dompage *d)
{
    if (!d) return;
    js_free(d->rt);
    for (int i = 0; i < d->nnodes; i++) {
        dnode *n = d->nodes[i];
        for (int a = 0; a < n->nattrs; a++) JS_FREE(n->attrs[a].val);
        if (n->attrs) JS_FREE(n->attrs);
        if (n->text) JS_FREE(n->text);
        if (n->ls) JS_FREE(n->ls);
        JS_FREE(n);
    }
    if (d->nodes) JS_FREE(d->nodes);
    if (d->timers) JS_FREE(d->timers);
    if (d->wls) JS_FREE(d->wls);
    JS_FREE(d);
}

static void maybe_gc(dompage *d)
{
    if (d->rt->bytes > d->rt->gc_mark_bytes) gc_run(d->rt);
}

static int take_dirty(dompage *d)
{
    int r = d->dirty ? DOM_CHANGED : 0;
    d->dirty = 0;
    return r;
}

static dnode *next_in_doc(dnode *n)
{
    if (n->first) return n->first;
    while (n) { if (n->next) return n->next; n = n->parent; }
    return NULL;
}

int dom_run_scripts(dompage *d)
{
    js_rt *rt = d->rt;
    RT_ENTER(rt);
    d->ready = 0;
    for (dnode *n = d->doc; n; n = next_in_doc(n)) {
        if (!dn_is(n, "script")) continue;
        const char *type = dn_attr_raw(n, "type");
        if (type && type[0] && strcmp(type, "text/javascript") && strcmp(type, "application/javascript") && strcmp(type, "text/ecmascript")) continue;
        if (dn_attr_raw(n, "src")) {
            sbuf b; sb_init(&b);
            sb_adds(&b, "script src is not loaded yet: "); sb_adds(&b, dn_attr_raw(n, "src"));
            host_log(d, b.p, b.len);
            sb_free(&b);
            continue;
        }
        sbuf code; sb_init(&code);
        for (dnode *k = n->first; k; k = k->next) if (k->type == DN_TEXT && k->text) sb_add(&code, k->text, k->tlen);
        d->cur_script = n;
        char err[300];
        err[0] = 0;
        rt->throwing = 0;
        if (js_eval(rt, code.p ? code.p : "", (int)code.len, err, sizeof err)) host_log(d, err, (unsigned)strlen(err));
        d->cur_script = NULL;
        sb_free(&code);
    }
    d->ready = 1;
    d->depth = 1;
    jsobj *ev = make_event(d, "DOMContentLoaded", 1);
    dispatch(d, d->doc, ev, "DOMContentLoaded", 1);
    d->ready = 2;
    ev = make_event(d, "load", 0);
    dispatch(d, NULL, ev, "load", 0);
    d->depth = 0;
    RT_LEAVE(rt);
    maybe_gc(d);
    return take_dirty(d);
}

char *dom_serialize(dompage *d, int *len)
{
    sbuf b; sb_init(&b);
    dn_serialize(d, d->doc, &b, 1, 1);
    if (b.oom || !b.p) { sb_free(&b); return NULL; }
    if (len) *len = (int)b.len;
    return b.p;
}

void dom_str_free(char *s) { if (s) JS_FREE(s); }

static dnode *node_by_id(dompage *d, int id) { return id >= 1 && id <= d->nnodes ? d->nodes[id - 1] : NULL; }

int dom_click(dompage *d, int nid, char *nav, int navmax)
{
    dnode *n = node_by_id(d, nid);
    if (!n) return 0;
    js_rt *rt = d->rt;
    RT_ENTER(rt);
    int flags = 0;
    d->depth = 1;
    jsobj *ev = make_event(d, "click", 1);
    int prevented = dispatch(d, n, ev, "click", 1);
    d->depth = 0;
    if (prevented) flags |= DOM_PREVENTED;
    else {
        for (dnode *a = n; a; a = a->parent) {
            if (!dn_is(a, "a")) continue;
            const char *href = dn_attr_raw(a, "href");
            if (!href || !href[0] || href[0] == '#') break;
            sbuf dec; sb_init(&dec);
            html_decode(href, (unsigned)strlen(href), &dec);
            if (dec.p && !strncmp(dec.p, "javascript:", 11)) {
                jsval fn = compile_handler(d, dec.p + 11, dec.len - 11);
                d->depth = 1;
                if (is_callable(fn)) call_safe(d, fn, dn_wrap(d, a), NULL, 0);
                d->depth = 0;
            } else if (nav && navmax > 0 && dec.p) { strlcpy(nav, dec.p, (size_t)navmax); flags |= DOM_NAVIGATE; }
            sb_free(&dec);
            break;
        }
    }
    RT_LEAVE(rt);
    maybe_gc(d);
    return flags | take_dirty(d);
}

int dom_input(dompage *d, int nid)
{
    dnode *n = node_by_id(d, nid);
    if (!n) return 0;
    js_rt *rt = d->rt;
    RT_ENTER(rt);
    d->depth = 1;
    jsobj *ev = make_event(d, "input", 1);
    dispatch(d, n, ev, "input", 1);
    ev = make_event(d, "change", 1);
    dispatch(d, n, ev, "change", 1);
    d->depth = 0;
    RT_LEAVE(rt);
    maybe_gc(d);
    return take_dirty(d);
}

int dom_submit(dompage *d, int form_nid)
{
    dnode *n = node_by_id(d, form_nid);
    if (!n) return 0;
    js_rt *rt = d->rt;
    RT_ENTER(rt);
    d->depth = 1;
    jsobj *ev = make_event(d, "submit", 1);
    int prevented = dispatch(d, n, ev, "submit", 1);
    d->depth = 0;
    RT_LEAVE(rt);
    maybe_gc(d);
    return (prevented ? DOM_PREVENTED : 0) | take_dirty(d);
}

void dom_set_field(dompage *d, int nid, const char *value, int checked)
{
    dnode *n = node_by_id(d, nid);
    if (!n || n->type != DN_ELEMENT) return;
    int was = d->dirty;
    sbuf enc; sb_init(&enc);
    if (dn_is(n, "textarea") && value) {
        html_encode(value, (unsigned)strlen(value), &enc, 0);
        dn_clear_children(d, n);
        if (enc.len) { dnode *t = dn_new(d, DN_TEXT, atom_c(d->rt, "#text")); dn_set_text(d, t, enc.p, enc.len); dn_append(d, n, t); }
    } else if (value) {
        html_encode(value, (unsigned)strlen(value), &enc, 1);
        dn_set_attr_raw(d, n, atom_c(d->rt, "value"), enc.p ? enc.p : "", enc.len);
    }
    sb_free(&enc);
    jstr *ck = atom_c(d->rt, "checked");
    const char *ty = dn_attr_raw(n, "type");
    if (ty && (!strcmp(ty, "checkbox") || !strcmp(ty, "radio"))) {
        if (checked) dn_set_attr_raw(d, n, ck, "", 0); else dn_del_attr(d, n, ck);
    }
    d->dirty = was;                                          /* what the user typed is not a script change */
}

int dom_poll(dompage *d)
{
    js_rt *rt = d->rt;
    long long now = d->host.now_ms ? d->host.now_ms(d->host.ud) : 0;
    int ran = 0;
    RT_ENTER(rt);
    d->depth = 1;
    for (int i = 0; i < d->ntimers && ran < 20; i++) {
        struct dtimer *t = &d->timers[i];
        if (!t->active || t->due > now) continue;
        jsval fn = d->timer_fns->items[i];
        if (t->interval > 0) t->due = now + t->interval;
        else { t->active = 0; d->timer_fns->items[i] = v_undef(); }
        ran++;
        if (is_callable(fn)) {
            jsval a[1] = { vnum(now) };
            call_safe(d, fn, v_obj(rt->global), a, 1);
        }
    }
    d->depth = 0;
    RT_LEAVE(rt);
    maybe_gc(d);
    return take_dirty(d);
}

int dom_has_timers(dompage *d)
{
    for (int i = 0; i < d->ntimers; i++) if (d->timers[i].active) return 1;
    return 0;
}
