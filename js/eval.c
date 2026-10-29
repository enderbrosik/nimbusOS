/* NimbusOS JavaScript engine - tree-walking evaluator. */
#include "js/js_int.h"

enum { C_NORMAL, C_BREAK, C_CONTINUE, C_RETURN, C_THROW };

static jsval eval(js_rt *rt, node *n, jsobj *sc);
static int exec_stmt(js_rt *rt, node *n, jsobj *sc);

#define BAIL(r) do { if ((rt)->throwing || (rt)->abort_msg) return (r); } while (0)
#define U v_undef()

static int step(js_rt *rt)
{
    if (++rt->steps > rt->max_steps && !rt->abort_msg) rt->abort_msg = "script took too long (step limit)";
    return rt->abort_msg != NULL;
}

/* ---- errors ------------------------------------------------------------------------------------------------ */

jsval throw_error(js_rt *rt, const char *kind, const char *m1, const char *m2, const char *m3)
{
    if (rt->throwing || rt->abort_msg) return U;
    jsobj *proto = rt->proto_error;
    prop *ctor = prop_find(rt->global, atom_c(rt, kind));
    if (ctor && ctor->val.t == JT_OBJ) {
        prop *pp = prop_find(ctor->val.u.o, rt->a_prototype);
        if (pp && pp->val.t == JT_OBJ) proto = pp->val.u.o;
    }
    jsobj *e = obj_new(rt, proto, CL_ERROR);
    sbuf b; sb_init(&b);
    if (m1) sb_adds(&b, m1);
    if (m2) sb_adds(&b, m2);
    if (m3) sb_adds(&b, m3);
    prop_put(rt, e, rt->a_message, v_str(sb_str(rt, &b)), PF_HIDDEN);
    sb_free(&b);
    rt->throwing = 1;
    rt->exc = v_obj(e);
    return U;
}

/* ---- type conversion --------------------------------------------------------------------------------------- */

int is_callable(jsval v) { return v.t == JT_OBJ && v.u.o->cls == CL_FUNCTION; }

const char *type_of(jsval v)
{
    switch (v.t) {
    case JT_UNDEF: return "undefined";
    case JT_NULL: return "object";
    case JT_BOOL: return "boolean";
    case JT_NUM: return "number";
    case JT_STR: return "string";
    default: return v.u.o->cls == CL_FUNCTION ? "function" : "object";
    }
}

int to_bool(jsval v)
{
    switch (v.t) {
    case JT_UNDEF: case JT_NULL: return 0;
    case JT_BOOL: return v.u.b;
    case JT_NUM: return v.u.n != 0 && v.u.n != JS_NAN;
    case JT_STR: return v.u.s->len != 0;
    default: return 1;
    }
}

jsval to_prim(js_rt *rt, jsval v, int prefer_string)
{
    if (v.t != JT_OBJ) return v;
    jstr *order[2];
    order[0] = prefer_string ? rt->a_tostring : rt->a_valueof;
    order[1] = prefer_string ? rt->a_valueof : rt->a_tostring;
    for (int i = 0; i < 2; i++) {
        jsval f = get_prop(rt, v, order[i]);
        BAIL(U);
        if (is_callable(f)) {
            jsval r = call_fn(rt, f, v, NULL, 0);
            BAIL(U);
            if (r.t != JT_OBJ) return r;
        }
    }
    return throw_error(rt, "TypeError", "Cannot convert object to primitive value", NULL, NULL);
}

jstr *to_str(js_rt *rt, jsval v)
{
    char buf[40];
    switch (v.t) {
    case JT_UNDEF: return rt->a_undefined;
    case JT_NULL: return rt->a_null;
    case JT_BOOL: return v.u.b ? rt->a_true : rt->a_false;
    case JT_NUM: { int n = num_fmt(v.u.n, buf); return str_new(rt, buf, (unsigned)n); }
    case JT_STR: return v.u.s;
    default: {
        jsval p = to_prim(rt, v, 1);
        if (NOVAL(rt)) return rt->a_empty;
        return to_str(rt, p);
    }
    }
}

js_num to_num(js_rt *rt, jsval v)
{
    switch (v.t) {
    case JT_UNDEF: return JS_NAN;
    case JT_NULL: return 0;
    case JT_BOOL: return v.u.b ? JS_SCALE : 0;
    case JT_NUM: return v.u.n;
    case JT_STR: return num_parse(v.u.s->s, (int)v.u.s->len);
    default: {
        jsval p = to_prim(rt, v, 0);
        if (NOVAL(rt)) return JS_NAN;
        return to_num(rt, p);
    }
    }
}

jstr *to_key_atom(js_rt *rt, jsval v)
{
    if (v.t == JT_STR) return to_atom(rt, v.u.s);
    jstr *s = to_str(rt, v);
    return to_atom(rt, s);
}

int strict_eq(jsval a, jsval b)
{
    if (a.t != b.t) return 0;
    switch (a.t) {
    case JT_UNDEF: case JT_NULL: return 1;
    case JT_BOOL: return a.u.b == b.u.b;
    case JT_NUM: return a.u.n == b.u.n && a.u.n != JS_NAN;
    case JT_STR: return str_eq(a.u.s, b.u.s);
    default: return a.u.o == b.u.o;
    }
}

int loose_eq(js_rt *rt, jsval a, jsval b)
{
    if (a.t == b.t) return strict_eq(a, b);
    if ((a.t == JT_UNDEF || a.t == JT_NULL) && (b.t == JT_UNDEF || b.t == JT_NULL)) return 1;
    if (a.t == JT_UNDEF || a.t == JT_NULL || b.t == JT_UNDEF || b.t == JT_NULL) return 0;
    if (a.t == JT_OBJ) { a = to_prim(rt, a, 0); if (NOVAL(rt)) return 0; return loose_eq(rt, a, b); }
    if (b.t == JT_OBJ) { b = to_prim(rt, b, 0); if (NOVAL(rt)) return 0; return loose_eq(rt, a, b); }
    js_num x = to_num(rt, a), y = to_num(rt, b);
    return x == y && x != JS_NAN;
}

/* ---- property access --------------------------------------------------------------------------------------- */

static jsval lookup_chain(jsobj *o, jstr *key)
{
    for (; o; o = o->proto) {
        prop *p = prop_find(o, key);
        if (p) return p->val;
    }
    return v_undef();
}

static int has_chain(jsobj *o, jstr *key)
{
    for (; o; o = o->proto) if (prop_find(o, key)) return 1;
    return 0;
}

static jsval fn_prototype(js_rt *rt, jsobj *f)
{
    prop *p = prop_find(f, rt->a_prototype);
    if (p) return p->val;
    if (f->native || f->is_arrow) return U;
    jsobj *proto = obj_new(rt, rt->proto_object, CL_OBJECT);
    prop_put(rt, proto, rt->a_constructor, v_obj(f), PF_HIDDEN);
    prop_put(rt, f, rt->a_prototype, v_obj(proto), PF_HIDDEN);
    return v_obj(proto);
}

jsval get_prop(js_rt *rt, jsval base, jstr *key)
{
    switch (base.t) {
    case JT_OBJ: {
        jsobj *o = base.u.o;
        if (o->cls == CL_ARRAY) {
            if (key == rt->a_length) return v_num(num_from_int(o->alen));
            unsigned idx;
            if (atom_index(key, &idx)) return idx < o->alen ? o->items[idx] : U;
        } else if (o->cls == CL_FUNCTION) {
            if (key == rt->a_prototype) return fn_prototype(rt, o);
            if (!prop_find(o, key)) {
                if (key == rt->a_name) return o->fnode && o->fnode->str ? v_str(o->fnode->str) : v_str(rt->a_empty);
                if (key == rt->a_length) return v_num(num_from_int(o->fnode ? o->fnode->nparams : 0));
            }
        }
        return lookup_chain(o, key);
    }
    case JT_STR: {
        jstr *s = base.u.s;
        if (key == rt->a_length) return v_num(num_from_int(s->len));
        unsigned idx;
        if (atom_index(key, &idx)) return idx < s->len ? v_str(str_new(rt, s->s + idx, 1)) : U;
        return lookup_chain(rt->proto_string, key);
    }
    case JT_NUM: return lookup_chain(rt->proto_number, key);
    case JT_BOOL: return lookup_chain(rt->proto_boolean, key);
    default: {
        sbuf b; sb_init(&b);
        sb_adds(&b, "Cannot read properties of "); sb_adds(&b, base.t == JT_NULL ? "null" : "undefined");
        sb_adds(&b, " (reading '"); sb_add(&b, key->s, key->len); sb_adds(&b, "')");
        throw_error(rt, "TypeError", b.p, NULL, NULL);
        sb_free(&b);
        return U;
    }
    }
}

jsval get_member(js_rt *rt, jsval base, jsval key)
{
    if (key.t == JT_NUM && num_isfin(key.u.n) && key.u.n >= 0 && key.u.n % JS_SCALE == 0) {
        long long idx = key.u.n / JS_SCALE;
        if (base.t == JT_OBJ && base.u.o->cls == CL_ARRAY)
            return idx < (long long)base.u.o->alen ? base.u.o->items[idx] : U;
        if (base.t == JT_STR)
            return idx < (long long)base.u.s->len ? v_str(str_new(rt, base.u.s->s + idx, 1)) : U;
    }
    if (base.t == JT_UNDEF || base.t == JT_NULL) {
        jstr *k = to_str(rt, key);
        return get_prop(rt, base, to_atom(rt, k));
    }
    jstr *k = to_key_atom(rt, key);
    BAIL(U);
    return get_prop(rt, base, k);
}

void set_prop(js_rt *rt, jsval base, jstr *key, jsval v)
{
    if (base.t == JT_UNDEF || base.t == JT_NULL) {
        sbuf b; sb_init(&b);
        sb_adds(&b, "Cannot set properties of "); sb_adds(&b, base.t == JT_NULL ? "null" : "undefined");
        sb_adds(&b, " (setting '"); sb_add(&b, key->s, key->len); sb_adds(&b, "')");
        throw_error(rt, "TypeError", b.p, NULL, NULL);
        sb_free(&b);
        return;
    }
    if (base.t != JT_OBJ) return;                              /* assignments to primitives are ignored */
    jsobj *o = base.u.o;
    if (o->cls == CL_ARRAY) {
        unsigned idx;
        if (key == rt->a_length) {
            js_num n = to_num(rt, v);
            if (!num_isfin(n) || n < 0 || n % JS_SCALE) { throw_error(rt, "RangeError", "Invalid array length", NULL, NULL); return; }
            if (n / JS_SCALE > 0x1000000) { throw_error(rt, "RangeError", "Invalid array length", NULL, NULL); return; }
            arr_set_len(rt, o, (unsigned)(n / JS_SCALE));
            return;
        }
        if (atom_index(key, &idx)) {
            if (idx > o->alen + 0x100000u) { throw_error(rt, "RangeError", "Array index too large", NULL, NULL); return; }
            if (idx >= o->alen) arr_set_len(rt, o, idx + 1);
            if (idx < o->alen) o->items[idx] = v;
            return;
        }
    }
    if (!o->extensible && !prop_find(o, key)) return;
    prop *p = prop_find(o, key);
    if (p && (p->flags & PF_CONST)) return;                    /* read-only: ignored, as in sloppy mode */
    prop_put(rt, o, key, v, 0);
}

void set_member(js_rt *rt, jsval base, jsval key, jsval v)
{
    if (key.t == JT_NUM && num_isfin(key.u.n) && key.u.n >= 0 && key.u.n % JS_SCALE == 0 && base.t == JT_OBJ &&
        base.u.o->cls == CL_ARRAY) {
        long long idx = key.u.n / JS_SCALE;
        jsobj *a = base.u.o;
        if (idx < (long long)a->alen) { a->items[idx] = v; return; }
        if (idx <= (long long)a->alen + 0x100000) {
            arr_set_len(rt, a, (unsigned)idx + 1);
            if (a->alen == (unsigned)idx + 1) a->items[idx] = v;
            return;
        }
    }
    jstr *k = to_key_atom(rt, key);
    if (NOVAL(rt)) return;
    set_prop(rt, base, k, v);
}

/* ---- scopes ------------------------------------------------------------------------------------------------ */

static jsobj *new_scope(js_rt *rt, jsobj *outer)
{
    jsobj *s = obj_new(rt, NULL, CL_SCOPE);
    s->outer = outer;
    return s;
}

static prop *scope_find(jsobj *sc, jstr *name, jsobj **owner)
{
    for (; sc; sc = sc->outer) {
        prop *p = prop_find(sc, name);
        if (p) { if (owner) *owner = sc; return p; }
    }
    return NULL;
}

static void declare(js_rt *rt, jsobj *sc, jstr *name, jsval v, int kind)
{
    prop *p = prop_find(sc, name);
    if (p) { p->val = v; if (kind == VK_CONST) p->flags |= PF_CONST; return; }
    prop_put(rt, sc, name, v, kind == VK_CONST ? PF_CONST : 0);
}

static void assign_var(js_rt *rt, jsobj *sc, jstr *name, jsval v)
{
    prop *p = scope_find(sc, name, NULL);
    if (p) {
        if (p->flags & PF_CONST) { throw_error(rt, "TypeError", "Assignment to constant variable.", NULL, NULL); return; }
        p->val = v;
        return;
    }
    prop_put(rt, rt->global, name, v, 0);                      /* undeclared: becomes a global (sloppy mode) */
}

static jsval this_value(js_rt *rt, jsobj *sc)
{
    for (; sc; sc = sc->outer) if (sc->has_this) return sc->thisv;
    return v_obj(rt->global);
}

jsobj *make_closure(js_rt *rt, node *f, jsobj *scope)
{
    jsobj *c = obj_new(rt, rt->proto_function, CL_FUNCTION);
    c->fnode = f;
    c->env = scope;
    c->is_arrow = f->flag;
    return c;
}

/* ---- calls ------------------------------------------------------------------------------------------------- */

static void describe(node *n, sbuf *b, int depth)
{
    if (depth > 4) { sb_adds(b, "..."); return; }
    switch (n->type) {
    case N_IDENT: sb_add(b, n->str->s, n->str->len); break;
    case N_THIS: sb_adds(b, "this"); break;
    case N_MEMBER: describe(n->a, b, depth + 1); sb_addc(b, '.'); sb_add(b, n->str->s, n->str->len); break;
    case N_INDEX: describe(n->a, b, depth + 1); sb_adds(b, "[...]"); break;
    case N_CALL: describe(n->a, b, depth + 1); sb_adds(b, "(...)"); break;
    default: sb_adds(b, "expression");
    }
}

jsval call_fn(js_rt *rt, jsval fnv, jsval thisv, jsval *args, int nargs)
{
    if (!is_callable(fnv)) return throw_error(rt, "TypeError", "value is not a function", NULL, NULL);
    jsobj *f = fnv.u.o;
    if (rt->depth >= rt->max_depth) return throw_error(rt, "RangeError", "Maximum call stack size exceeded", NULL, NULL);
    if (step(rt)) return U;
    rt->depth++;
    jsval result = U;
    if (f->bound_fn) {
        int total = f->nbound + nargs;
        jsval *all = (jsval *)JS_MALLOC((size_t)(total ? total : 1) * sizeof(jsval));
        if (!all) rt->abort_msg = "out of memory";
        else {
            for (int i = 0; i < f->nbound; i++) all[i] = f->bound_args[i];
            for (int i = 0; i < nargs; i++) all[f->nbound + i] = args[i];
            result = call_fn(rt, v_obj(f->bound_fn), f->bound_this, all, total);
            JS_FREE(all);
        }
    } else if (f->native) {
        rt->cur_magic = f->magic;
        result = f->native(rt, thisv, args, nargs);
    } else {
        node *fn = f->fnode;
        jsobj *sc = new_scope(rt, f->env);
        if (!f->is_arrow) {
            sc->has_this = 1;
            sc->thisv = (thisv.t == JT_UNDEF || thisv.t == JT_NULL) ? v_obj(rt->global) : thisv;
        }
        if (fn->str && !prop_find(sc, fn->str)) prop_put(rt, sc, fn->str, fnv, 0);
        int i = 0;
        for (node *id = fn->params; id; id = id->next, i++) {
            jsval v = i < nargs ? args[i] : U;
            if (v.t == JT_UNDEF && id->a) { v = eval(rt, id->a, sc); if (NOVAL(rt)) break; }
            prop_put(rt, sc, id->str, v, 0);
        }
        if (!NOVAL(rt)) {
            if (fn->uses_args && !f->is_arrow) {
                jsobj *a = arr_new(rt, 0);
                for (int k = 0; k < nargs; k++) arr_push(rt, a, args[k]);
                prop_put(rt, sc, rt->a_arguments, v_obj(a), 0);
            }
            for (node *v = fn->vars; v; v = v->next) if (!prop_find(sc, v->str)) prop_put(rt, sc, v->str, U, 0);
            for (node *h = fn->funcs; h; h = h->next) prop_put(rt, sc, h->str, v_obj(make_closure(rt, h->a, sc)), 0);
            if (fn->expr_body) {
                result = eval(rt, fn->body, sc);
            } else {
                for (node *s = fn->body; s; s = s->next) {
                    int c = exec_stmt(rt, s, sc);
                    if (c == C_RETURN) { result = rt->ret; rt->ret = U; break; }
                    if (c == C_THROW) break;
                }
            }
        }
    }
    rt->depth--;
    if (rt->throwing) return U;
    return result;
}

jsval construct(js_rt *rt, jsval fnv, jsval *args, int nargs)
{
    if (!is_callable(fnv) || fnv.u.o->is_arrow) return throw_error(rt, "TypeError", "value is not a constructor", NULL, NULL);
    jsobj *f = fnv.u.o;
    if (f->native) {
        rt->cur_new = 1;
        jsval r = call_fn(rt, fnv, U, args, nargs);
        rt->cur_new = 0;
        return r;
    }
    jsval pv = get_prop(rt, fnv, rt->a_prototype);
    BAIL(U);
    jsobj *o = obj_new(rt, pv.t == JT_OBJ ? pv.u.o : rt->proto_object, CL_OBJECT);
    jsval r = call_fn(rt, fnv, v_obj(o), args, nargs);
    BAIL(U);
    return r.t == JT_OBJ ? r : v_obj(o);
}

/* ---- operators --------------------------------------------------------------------------------------------- */

static int compare_values(js_rt *rt, jsval a, jsval b, int *unordered)
{
    *unordered = 0;
    a = to_prim(rt, a, 0); BAIL(0);
    b = to_prim(rt, b, 0); BAIL(0);
    if (a.t == JT_STR && b.t == JT_STR) {
        unsigned n = a.u.s->len < b.u.s->len ? a.u.s->len : b.u.s->len;
        int c = memcmp(a.u.s->s, b.u.s->s, n);
        if (c) return c < 0 ? -1 : 1;
        return a.u.s->len < b.u.s->len ? -1 : a.u.s->len > b.u.s->len ? 1 : 0;
    }
    js_num x = to_num(rt, a), y = to_num(rt, b);
    BAIL(0);
    int c = num_cmp(x, y);
    if (c == 2) { *unordered = 1; return 0; }
    return c;
}

static int instance_of(js_rt *rt, jsval v, jsval ctor)
{
    if (!is_callable(ctor)) { throw_error(rt, "TypeError", "Right-hand side of 'instanceof' is not callable", NULL, NULL); return 0; }
    if (v.t != JT_OBJ) return 0;
    jsval pv = get_prop(rt, ctor, rt->a_prototype);
    BAIL(0);
    if (pv.t != JT_OBJ) return 0;
    for (jsobj *o = v.u.o->proto; o; o = o->proto) if (o == pv.u.o) return 1;
    return 0;
}

static int has_property(js_rt *rt, jsval obj, jsval key)
{
    if (obj.t != JT_OBJ) { throw_error(rt, "TypeError", "Cannot use 'in' operator to search for a key in a non-object", NULL, NULL); return 0; }
    jstr *k = to_key_atom(rt, key);
    BAIL(0);
    jsobj *o = obj.u.o;
    unsigned idx;
    if (o->cls == CL_ARRAY) {
        if (k == rt->a_length) return 1;
        if (atom_index(k, &idx)) return idx < o->alen;
    }
    return has_chain(o, k);
}

static jsval binary_op(js_rt *rt, int op, jsval a, jsval b)
{
    switch (op) {
    case O_ADD: {
        a = to_prim(rt, a, 0); BAIL(U);
        b = to_prim(rt, b, 0); BAIL(U);
        if (a.t == JT_STR || b.t == JT_STR) {
            jstr *x = to_str(rt, a), *y = to_str(rt, b);
            BAIL(U);
            return v_str(str_cat(rt, x, y));
        }
        js_num x = to_num(rt, a), y = to_num(rt, b);
        return v_num(num_add(x, y));
    }
    case O_SUB: case O_MUL: case O_DIV: case O_MOD: case O_POW: {
        js_num x = to_num(rt, a); BAIL(U);
        js_num y = to_num(rt, b); BAIL(U);
        switch (op) {
        case O_SUB: return v_num(num_sub(x, y));
        case O_MUL: return v_num(num_mul(x, y));
        case O_DIV: return v_num(num_div(x, y));
        case O_MOD: return v_num(num_mod(x, y));
        default: return v_num(num_pow(x, y));
        }
    }
    case O_SHL: case O_SHR: case O_USHR: case O_BAND: case O_BOR: case O_BXOR: {
        js_num x = to_num(rt, a); BAIL(U);
        js_num y = to_num(rt, b); BAIL(U);
        int xi = num_to_int32(x), yi = num_to_int32(y);
        long long r;
        switch (op) {
        case O_SHL: r = (int)((unsigned)xi << (yi & 31)); break;
        case O_SHR: r = xi >> (yi & 31); break;
        case O_USHR: r = (long long)((unsigned)xi >> (yi & 31)); break;
        case O_BAND: r = xi & yi; break;
        case O_BOR: r = xi | yi; break;
        default: r = xi ^ yi; break;
        }
        return v_num(num_from_int(r));
    }
    case O_EQ: { int r = loose_eq(rt, a, b); BAIL(U); return v_bool(r); }
    case O_NE: { int r = loose_eq(rt, a, b); BAIL(U); return v_bool(!r); }
    case O_SEQ: return v_bool(strict_eq(a, b));
    case O_SNE: return v_bool(!strict_eq(a, b));
    case O_LT: case O_GT: case O_LE: case O_GE: {
        int un, c = compare_values(rt, a, b, &un);
        BAIL(U);
        if (un) return v_bool(0);
        return v_bool(op == O_LT ? c < 0 : op == O_GT ? c > 0 : op == O_LE ? c <= 0 : c >= 0);
    }
    case O_IN: { int r = has_property(rt, b, a); BAIL(U); return v_bool(r); }
    case O_INSTANCEOF: { int r = instance_of(rt, a, b); BAIL(U); return v_bool(r); }
    }
    return U;
}

/* ---- keys -------------------------------------------------------------------------------------------------- */

jsobj *own_keys(js_rt *rt, jsobj *o, int all)
{
    jsobj *out = arr_new(rt, 0);
    char buf[16];
    if (o->cls == CL_ARRAY)
        for (unsigned i = 0; i < o->alen; i++) {
            int n = num_fmt(num_from_int(i), buf);
            arr_push(rt, out, v_str(str_new(rt, buf, (unsigned)n)));
        }
    if (o->cls != CL_SCOPE) {                                   /* integer-like keys come first, in ascending order */
        unsigned prev = 0; int have_prev = 0;
        for (;;) {
            int best = -1; unsigned bi = 0;
            for (int i = 0; i < o->np; i++) {
                unsigned idx;
                if ((!all && (o->props[i].flags & PF_HIDDEN)) || !atom_index(o->props[i].key, &idx)) continue;
                if (have_prev && idx <= prev) continue;
                if (best < 0 || idx < bi) { best = i; bi = idx; }
            }
            if (best < 0) break;
            arr_push(rt, out, v_str(o->props[best].key));
            prev = bi; have_prev = 1;
        }
    }
    for (int i = 0; i < o->np; i++) {
        unsigned idx;
        if (!all && (o->props[i].flags & PF_HIDDEN)) continue;
        if (o->cls != CL_SCOPE && atom_index(o->props[i].key, &idx)) continue;
        arr_push(rt, out, v_str(o->props[i].key));
    }
    return out;
}

/* ---- expressions ------------------------------------------------------------------------------------------- */

static jsval eval_args_call(js_rt *rt, node *call, jsobj *sc);

static int spread_into(js_rt *rt, jsobj *dst, jsval src)
{
    if (src.t == JT_OBJ && src.u.o->cls == CL_ARRAY) {
        for (unsigned i = 0; i < src.u.o->alen; i++) arr_push(rt, dst, src.u.o->items[i]);
        return 1;
    }
    if (src.t == JT_STR) {
        const char *s = src.u.s->s; unsigned n = src.u.s->len;
        for (unsigned i = 0; i < n; ) {
            unsigned k = 1;
            unsigned char c = (unsigned char)s[i];
            if (c >= 0xF0) k = 4; else if (c >= 0xE0) k = 3; else if (c >= 0xC0) k = 2;
            if (i + k > n) k = n - i;
            arr_push(rt, dst, v_str(str_new(rt, s + i, k)));
            i += k;
        }
        return 1;
    }
    throw_error(rt, "TypeError", "value is not iterable", NULL, NULL);
    return 0;
}

static jsval update_value(js_rt *rt, node *n, jsval old)
{
    js_num x = to_num(rt, old);
    return v_num(n->op == O_INC ? num_add(x, JS_SCALE) : num_sub(x, JS_SCALE));
}

static jsval eval(js_rt *rt, node *n, jsobj *sc)
{
    if (step(rt)) return U;
    switch (n->type) {
    case N_NUM: return v_num(n->num);
    case N_STR: return v_str(n->str);
    case N_NULL: return v_null();
    case N_TRUE: return v_bool(1);
    case N_FALSE: return v_bool(0);
    case N_THIS: return this_value(rt, sc);
    case N_IDENT: {
        prop *p = scope_find(sc, n->str, NULL);
        if (p) return p->val;
        if (n->str == rt->a_undefined) return U;
        sbuf b; sb_init(&b); sb_add(&b, n->str->s, n->str->len); sb_adds(&b, " is not defined");
        throw_error(rt, "ReferenceError", b.p, NULL, NULL);
        sb_free(&b);
        return U;
    }
    case N_TMPL: {
        sbuf b; sb_init(&b);
        int i = 0;
        for (node *part = n->a; part; part = part->next, i++) {
            if (i % 2 == 0) { sb_add(&b, part->str->s, part->str->len); continue; }
            jsval v = eval(rt, part, sc);
            if (NOVAL(rt)) { sb_free(&b); return U; }
            jstr *s = to_str(rt, v);
            if (NOVAL(rt)) { sb_free(&b); return U; }
            sb_add(&b, s->s, s->len);
        }
        jstr *r = sb_str(rt, &b);
        sb_free(&b);
        return v_str(r);
    }
    case N_ARRAY: {
        jsobj *a = arr_new(rt, 0);
        for (node *e = n->a; e; e = e->next) {
            if (e->type == N_SPREAD) {
                jsval s = eval(rt, e->a, sc); BAIL(U);
                if (!spread_into(rt, a, s)) return U;
            } else {
                jsval v = eval(rt, e, sc); BAIL(U);
                arr_push(rt, a, v);
            }
        }
        return v_obj(a);
    }
    case N_OBJECT: {
        jsobj *o = obj_new(rt, rt->proto_object, CL_OBJECT);
        for (node *pr = n->a; pr; pr = pr->next) {
            jstr *key = pr->str;
            if (pr->a) { jsval k = eval(rt, pr->a, sc); BAIL(U); key = to_key_atom(rt, k); BAIL(U); }
            jsval v = eval(rt, pr->b, sc);
            BAIL(U);
            if (v.t == JT_OBJ && v.u.o->cls == CL_FUNCTION && v.u.o->fnode && !v.u.o->fnode->str) v.u.o->fnode->str = key;
            unsigned idx;
            (void)idx;
            prop_put(rt, o, key, v, 0);
        }
        return v_obj(o);
    }
    case N_FUNC: return v_obj(make_closure(rt, n, sc));
    case N_MEMBER: {
        jsval base = eval(rt, n->a, sc); BAIL(U);
        return get_prop(rt, base, n->str);
    }
    case N_INDEX: {
        jsval base = eval(rt, n->a, sc); BAIL(U);
        jsval key = eval(rt, n->b, sc); BAIL(U);
        return get_member(rt, base, key);
    }
    case N_CALL: return eval_args_call(rt, n, sc);
    case N_NEW: {
        jsval fn = eval(rt, n->a, sc); BAIL(U);
        int cap = 8, argc = 0;
        jsval stackargs[8], *args = stackargs, *heap = NULL;
        for (node *a = n->b; a; a = a->next) {
            if (a->type == N_SPREAD) {
                jsval s = eval(rt, a->a, sc);
                if (NOVAL(rt)) { if (heap) JS_FREE(heap); return U; }
                jsobj *tmp = arr_new(rt, 0);
                if (!spread_into(rt, tmp, s)) { if (heap) JS_FREE(heap); return U; }
                for (unsigned k = 0; k < tmp->alen; k++) {
                    if (argc == cap) { cap *= 2; jsval *nh = (jsval *)JS_MALLOC((size_t)cap * sizeof(jsval)); if (!nh) { rt->abort_msg = "out of memory"; if (heap) JS_FREE(heap); return U; } memcpy(nh, args, (size_t)argc * sizeof(jsval)); if (heap) JS_FREE(heap); heap = nh; args = nh; }
                    args[argc++] = tmp->items[k];
                }
                continue;
            }
            jsval v = eval(rt, a, sc);
            if (NOVAL(rt)) { if (heap) JS_FREE(heap); return U; }
            if (argc == cap) { cap *= 2; jsval *nh = (jsval *)JS_MALLOC((size_t)cap * sizeof(jsval)); if (!nh) { rt->abort_msg = "out of memory"; if (heap) JS_FREE(heap); return U; } memcpy(nh, args, (size_t)argc * sizeof(jsval)); if (heap) JS_FREE(heap); heap = nh; args = nh; }
            args[argc++] = v;
        }
        if (!is_callable(fn) || fn.u.o->is_arrow) {
            sbuf b; sb_init(&b); describe(n->a, &b, 0); sb_adds(&b, " is not a constructor");
            throw_error(rt, "TypeError", b.p, NULL, NULL);
            sb_free(&b);
            if (heap) JS_FREE(heap);
            return U;
        }
        jsval r = construct(rt, fn, args, argc);
        if (heap) JS_FREE(heap);
        return r;
    }
    case N_UNARY: {
        if (n->op == O_TYPEOF) {
            if (n->a->type == N_IDENT) {
                prop *p = scope_find(sc, n->a->str, NULL);
                if (!p) return v_str(rt->a_undefined);
                return v_str(atom_c(rt, type_of(p->val)));
            }
            jsval v = eval(rt, n->a, sc); BAIL(U);
            return v_str(atom_c(rt, type_of(v)));
        }
        if (n->op == O_DELETE) {
            if (n->a->type == N_MEMBER || n->a->type == N_INDEX) {
                jsval base = eval(rt, n->a->a, sc); BAIL(U);
                jstr *key;
                if (n->a->type == N_MEMBER) key = n->a->str;
                else { jsval k = eval(rt, n->a->b, sc); BAIL(U); key = to_key_atom(rt, k); BAIL(U); }
                if (base.t == JT_OBJ) {
                    unsigned idx;
                    if (base.u.o->cls == CL_ARRAY && atom_index(key, &idx)) { if (idx < base.u.o->alen) base.u.o->items[idx] = U; return v_bool(1); }
                    prop_del(base.u.o, key);
                }
                return v_bool(1);
            }
            eval(rt, n->a, sc);
            return v_bool(1);
        }
        jsval v = eval(rt, n->a, sc); BAIL(U);
        switch (n->op) {
        case O_NOT: return v_bool(!to_bool(v));
        case O_VOID: return U;
        case O_PLUS: return v_num(to_num(rt, v));
        case O_MINUS: { js_num x = to_num(rt, v); return v_num(num_neg(x)); }
        case O_BNOT: { js_num x = to_num(rt, v); return v_num(num_from_int(~num_to_int32(x))); }
        }
        return U;
    }
    case N_UPDATE: {
        node *t = n->a;
        if (t->type == N_IDENT) {
            prop *p = scope_find(sc, t->str, NULL);
            if (!p) { sbuf b; sb_init(&b); sb_add(&b, t->str->s, t->str->len); sb_adds(&b, " is not defined"); throw_error(rt, "ReferenceError", b.p, NULL, NULL); sb_free(&b); return U; }
            jsval old = v_num(to_num(rt, p->val)); BAIL(U);
            jsval nv = update_value(rt, n, old);
            assign_var(rt, sc, t->str, nv); BAIL(U);
            return n->flag ? nv : old;
        }
        jsval base = eval(rt, t->a, sc); BAIL(U);
        jsval key;
        if (t->type == N_MEMBER) key = v_str(t->str);
        else { key = eval(rt, t->b, sc); BAIL(U); }
        jsval cur = get_member(rt, base, key); BAIL(U);
        jsval old = v_num(to_num(rt, cur)); BAIL(U);
        jsval nv = update_value(rt, n, old);
        set_member(rt, base, key, nv); BAIL(U);
        return n->flag ? nv : old;
    }
    case N_BINARY: {
        jsval a = eval(rt, n->a, sc); BAIL(U);
        jsval b = eval(rt, n->b, sc); BAIL(U);
        return binary_op(rt, n->op, a, b);
    }
    case N_LOGICAL: {
        jsval a = eval(rt, n->a, sc); BAIL(U);
        if (n->op == O_AND) return to_bool(a) ? eval(rt, n->b, sc) : a;
        if (n->op == O_OR) return to_bool(a) ? a : eval(rt, n->b, sc);
        return (a.t == JT_UNDEF || a.t == JT_NULL) ? eval(rt, n->b, sc) : a;
    }
    case N_COND: {
        jsval c = eval(rt, n->a, sc); BAIL(U);
        return eval(rt, to_bool(c) ? n->b : n->c, sc);
    }
    case N_SEQ: {
        eval(rt, n->a, sc); BAIL(U);
        return eval(rt, n->b, sc);
    }
    case N_ASSIGN: {
        node *t = n->a;
        if (t->type == N_IDENT) {
            jsval v;
            if (n->op == O_ASSIGN) { v = eval(rt, n->b, sc); BAIL(U); if (v.t == JT_OBJ && v.u.o->cls == CL_FUNCTION && v.u.o->fnode && !v.u.o->fnode->str && n->b->type == N_FUNC) v.u.o->fnode->str = t->str; }
            else {
                prop *p = scope_find(sc, t->str, NULL);
                if (!p) { sbuf b; sb_init(&b); sb_add(&b, t->str->s, t->str->len); sb_adds(&b, " is not defined"); throw_error(rt, "ReferenceError", b.p, NULL, NULL); sb_free(&b); return U; }
                jsval cur = p->val;
                jsval r = eval(rt, n->b, sc); BAIL(U);
                v = binary_op(rt, n->op, cur, r); BAIL(U);
            }
            assign_var(rt, sc, t->str, v); BAIL(U);
            return v;
        }
        jsval base = eval(rt, t->a, sc); BAIL(U);
        jsval key;
        if (t->type == N_MEMBER) key = v_str(t->str);
        else { key = eval(rt, t->b, sc); BAIL(U); }
        jsval v;
        if (n->op == O_ASSIGN) { v = eval(rt, n->b, sc); BAIL(U); }
        else {
            jsval cur = get_member(rt, base, key); BAIL(U);
            jsval r = eval(rt, n->b, sc); BAIL(U);
            v = binary_op(rt, n->op, cur, r); BAIL(U);
        }
        set_member(rt, base, key, v); BAIL(U);
        return v;
    }
    default:
        throw_error(rt, "SyntaxError", "Unsupported expression", NULL, NULL);
        return U;
    }
}

static jsval eval_args_call(js_rt *rt, node *n, jsobj *sc)
{
    node *callee = n->a;
    jsval fn, thisv = U;
    if (callee->type == N_MEMBER) {
        thisv = eval(rt, callee->a, sc); BAIL(U);
        fn = get_prop(rt, thisv, callee->str); BAIL(U);
    } else if (callee->type == N_INDEX) {
        thisv = eval(rt, callee->a, sc); BAIL(U);
        jsval key = eval(rt, callee->b, sc); BAIL(U);
        fn = get_member(rt, thisv, key); BAIL(U);
    } else {
        fn = eval(rt, callee, sc); BAIL(U);
    }
    int cap = 8, argc = 0;
    jsval stackargs[8], *args = stackargs, *heap = NULL;
    for (node *a = n->b; a; a = a->next) {
        jsval vals[1]; int nv = 1;
        jsobj *tmp = NULL;
        if (a->type == N_SPREAD) {
            jsval s = eval(rt, a->a, sc);
            if (NOVAL(rt)) { if (heap) JS_FREE(heap); return U; }
            tmp = arr_new(rt, 0);
            if (!spread_into(rt, tmp, s)) { if (heap) JS_FREE(heap); return U; }
            nv = (int)tmp->alen;
        } else {
            vals[0] = eval(rt, a, sc);
            if (NOVAL(rt)) { if (heap) JS_FREE(heap); return U; }
        }
        for (int k = 0; k < nv; k++) {
            if (argc == cap) {
                cap *= 2;
                jsval *nh = (jsval *)JS_MALLOC((size_t)cap * sizeof(jsval));
                if (!nh) { rt->abort_msg = "out of memory"; if (heap) JS_FREE(heap); return U; }
                memcpy(nh, args, (size_t)argc * sizeof(jsval));
                if (heap) JS_FREE(heap);
                heap = nh; args = nh;
            }
            args[argc++] = tmp ? tmp->items[k] : vals[0];
        }
    }
    jsval r;
    if (!is_callable(fn)) {
        sbuf b; sb_init(&b); describe(callee, &b, 0); sb_adds(&b, " is not a function");
        r = throw_error(rt, "TypeError", b.p, NULL, NULL);
        sb_free(&b);
    } else r = call_fn(rt, fn, thisv, args, argc);
    if (heap) JS_FREE(heap);
    return r;
}

/* ---- statements -------------------------------------------------------------------------------------------- */

static int exec_list(js_rt *rt, node *s, jsobj *sc)
{
    for (; s; s = s->next) {
        int c = exec_stmt(rt, s, sc);
        if (c != C_NORMAL) return c;
    }
    return C_NORMAL;
}

static jsobj *copy_scope(js_rt *rt, jsobj *from, jsobj *outer)
{
    jsobj *s = new_scope(rt, outer);
    for (int i = 0; i < from->np; i++) prop_put(rt, s, from->props[i].key, from->props[i].val, from->props[i].flags);
    return s;
}

static int bind_loop_var(js_rt *rt, node *decl, jsobj *sc, jsval v)
{
    if (decl->type == N_VAR) {
        node *d = decl->a;
        if (decl->flag == VK_VAR) assign_var(rt, sc, d->str, v);
        else declare(rt, sc, d->str, v, decl->flag);
    } else if (decl->type == N_IDENT) assign_var(rt, sc, decl->str, v);
    else { throw_error(rt, "SyntaxError", "Invalid left-hand side in for-loop", NULL, NULL); return 0; }
    return !NOVAL(rt);
}

static int exec_stmt(js_rt *rt, node *n, jsobj *sc)
{
    if (step(rt)) return C_THROW;
    switch (n->type) {
    case N_EMPTY: return C_NORMAL;
    case N_EXPR: eval(rt, n->a, sc); return NOVAL(rt) ? C_THROW : C_NORMAL;
    case N_VAR:
        for (node *d = n->a; d; d = d->next) {
            if (d->a) {
                jsval v = eval(rt, d->a, sc);
                if (NOVAL(rt)) return C_THROW;
                if (v.t == JT_OBJ && v.u.o->cls == CL_FUNCTION && v.u.o->fnode && !v.u.o->fnode->str && d->a->type == N_FUNC) v.u.o->fnode->str = d->str;
                if (n->flag == VK_VAR) assign_var(rt, sc, d->str, v);
                else declare(rt, sc, d->str, v, n->flag);
            } else if (n->flag != VK_VAR) declare(rt, sc, d->str, U, n->flag);
            if (NOVAL(rt)) return C_THROW;
        }
        return C_NORMAL;
    case N_BLOCK: {
        jsobj *bs = n->flag ? new_scope(rt, sc) : sc;
        return exec_list(rt, n->a, bs);
    }
    case N_IF: {
        jsval c = eval(rt, n->a, sc);
        if (NOVAL(rt)) return C_THROW;
        if (to_bool(c)) return exec_stmt(rt, n->b, sc);
        return n->c ? exec_stmt(rt, n->c, sc) : C_NORMAL;
    }
    case N_WHILE:
        for (;;) {
            jsval c = eval(rt, n->a, sc);
            if (NOVAL(rt)) return C_THROW;
            if (!to_bool(c)) break;
            int r = exec_stmt(rt, n->b, sc);
            if (r == C_BREAK) break;
            if (r == C_RETURN || r == C_THROW) return r;
        }
        return C_NORMAL;
    case N_DOWHILE:
        for (;;) {
            int r = exec_stmt(rt, n->b, sc);
            if (r == C_BREAK) break;
            if (r == C_RETURN || r == C_THROW) return r;
            jsval c = eval(rt, n->a, sc);
            if (NOVAL(rt)) return C_THROW;
            if (!to_bool(c)) break;
        }
        return C_NORMAL;
    case N_FOR: {
        jsobj *ls = n->flag ? new_scope(rt, sc) : sc;
        if (n->a) {
            if (n->a->type == N_VAR) { int r = exec_stmt(rt, n->a, ls); if (r != C_NORMAL) return r; }
            else { eval(rt, n->a, ls); if (NOVAL(rt)) return C_THROW; }
        }
        for (;;) {
            if (n->b) {
                jsval c = eval(rt, n->b, ls);
                if (NOVAL(rt)) return C_THROW;
                if (!to_bool(c)) break;
            }
            int r = exec_stmt(rt, n->d, ls);
            if (r == C_BREAK) break;
            if (r == C_RETURN || r == C_THROW) return r;
            if (n->flag) ls = copy_scope(rt, ls, sc);          /* each iteration gets its own copy of let variables */
            if (n->c) { eval(rt, n->c, ls); if (NOVAL(rt)) return C_THROW; }
        }
        return C_NORMAL;
    }
    case N_FORIN: {
        jsval ov = eval(rt, n->b, sc);
        if (NOVAL(rt)) return C_THROW;
        if (ov.t == JT_UNDEF || ov.t == JT_NULL) return C_NORMAL;
        jsobj *keys = arr_new(rt, 0);
        if (ov.t == JT_OBJ) {
            for (jsobj *o = ov.u.o; o; o = o->proto) {
                jsobj *k = own_keys(rt, o, 0);
                for (unsigned i = 0; i < k->alen; i++) {
                    int dup = 0;
                    for (unsigned j = 0; j < keys->alen; j++) if (str_eq(keys->items[j].u.s, k->items[i].u.s)) { dup = 1; break; }
                    if (!dup) arr_push(rt, keys, k->items[i]);
                }
            }
        } else if (ov.t == JT_STR) {
            char buf[16];
            for (unsigned i = 0; i < ov.u.s->len; i++) { int l = num_fmt(num_from_int(i), buf); arr_push(rt, keys, v_str(str_new(rt, buf, (unsigned)l))); }
        }
        for (unsigned i = 0; i < keys->alen; i++) {
            jsobj *ls = n->flag ? new_scope(rt, sc) : sc;
            if (!bind_loop_var(rt, n->a, ls, keys->items[i])) return C_THROW;
            int r = exec_stmt(rt, n->c, ls);
            if (r == C_BREAK) break;
            if (r == C_RETURN || r == C_THROW) return r;
        }
        return C_NORMAL;
    }
    case N_FOROF: {
        jsval ov = eval(rt, n->b, sc);
        if (NOVAL(rt)) return C_THROW;
        jsobj *items;
        if (ov.t == JT_OBJ && ov.u.o->cls == CL_ARRAY) items = ov.u.o;
        else {
            items = arr_new(rt, 0);
            if (!spread_into(rt, items, ov)) return C_THROW;
        }
        for (unsigned i = 0; i < items->alen; i++) {            /* arrays are read live: pushes during the loop are seen */
            jsobj *ls = n->flag ? new_scope(rt, sc) : sc;
            if (!bind_loop_var(rt, n->a, ls, items->items[i])) return C_THROW;
            int r = exec_stmt(rt, n->c, ls);
            if (r == C_BREAK) break;
            if (r == C_RETURN || r == C_THROW) return r;
        }
        return C_NORMAL;
    }
    case N_RETURN:
        if (n->a) { jsval v = eval(rt, n->a, sc); if (NOVAL(rt)) return C_THROW; rt->ret = v; }
        else rt->ret = U;
        return C_RETURN;
    case N_BREAK: return C_BREAK;
    case N_CONTINUE: return C_CONTINUE;
    case N_THROW: {
        jsval v = eval(rt, n->a, sc);
        if (NOVAL(rt)) return C_THROW;
        rt->throwing = 1;
        rt->exc = v;
        return C_THROW;
    }
    case N_TRY: {
        int c = exec_stmt(rt, n->a, sc);
        if (c == C_THROW && rt->throwing && !rt->abort_msg && n->b) {
            jsval ex = rt->exc;
            rt->throwing = 0;
            rt->exc = U;
            jsobj *cs = new_scope(rt, sc);
            if (n->b->str) prop_put(rt, cs, n->b->str, ex, 0);
            c = exec_stmt(rt, n->b->b, cs);
        }
        if (n->c && !rt->abort_msg) {
            int sthrow = rt->throwing; jsval sexc = rt->exc, sret = rt->ret;
            rt->throwing = 0;
            int fc = exec_stmt(rt, n->c, sc);
            if (fc != C_NORMAL) c = fc;                         /* return/break/throw in finally wins */
            else { rt->throwing = sthrow; rt->exc = sexc; rt->ret = sret; }
        }
        return c;
    }
    case N_SWITCH: {
        jsval d = eval(rt, n->a, sc);
        if (NOVAL(rt)) return C_THROW;
        jsobj *ss = n->flag ? new_scope(rt, sc) : sc;
        node *start = NULL;
        for (node *c = n->b; c && !start; c = c->next) {
            if (c->flag) continue;                              /* default */
            jsval t = eval(rt, c->a, ss);
            if (NOVAL(rt)) return C_THROW;
            if (strict_eq(d, t)) start = c;
        }
        if (!start) for (node *c = n->b; c; c = c->next) if (c->flag) { start = c; break; }
        for (node *c = start; c; c = c->next) {
            int r = exec_list(rt, c->b, ss);
            if (r == C_BREAK) return C_NORMAL;
            if (r != C_NORMAL) return r;
        }
        return C_NORMAL;
    }
    default:
        throw_error(rt, "SyntaxError", "Unsupported statement", NULL, NULL);
        return C_THROW;
    }
}

/* ---- running a program ------------------------------------------------------------------------------------- */

static void copy_err(char *err, int errmax, const char *a, const char *b)
{
    if (!err || errmax <= 0) return;
    int n = 0;
    while (a && *a && n < errmax - 1) err[n++] = *a++;
    while (b && *b && n < errmax - 1) err[n++] = *b++;
    err[n] = 0;
}

int js_run(js_rt *rt, node *prog, char *err, int errmax)
{
    rt->steps = 0;
    rt->depth = 0;
    rt->throwing = 0;
    jsobj *sc = rt->global;
    for (node *v = prog->vars; v; v = v->next) if (!prop_find(sc, v->str)) prop_put(rt, sc, v->str, U, 0);
    for (node *h = prog->funcs; h; h = h->next) prop_put(rt, sc, h->str, v_obj(make_closure(rt, h->a, sc)), 0);
    int c = C_NORMAL;
    for (node *s = prog->body; s && c == C_NORMAL; s = s->next) c = exec_stmt(rt, s, sc);
    if (rt->abort_msg) {
        copy_err(err, errmax, "Error: ", rt->abort_msg);
        rt->abort_msg = NULL;
        rt->throwing = 0;
        return 1;
    }
    if (rt->throwing) {
        sbuf b; sb_init(&b);
        sb_adds(&b, "Uncaught ");
        jsval ex = rt->exc;
        rt->throwing = 0;
        if (ex.t == JT_OBJ && ex.u.o->cls == CL_ERROR) {
            jsval nm = get_prop(rt, ex, rt->a_name), msg = get_prop(rt, ex, rt->a_message);
            jstr *ns = to_str(rt, nm), *ms = to_str(rt, msg);
            sb_add(&b, ns->s, ns->len);
            if (ms->len) { sb_adds(&b, ": "); sb_add(&b, ms->s, ms->len); }
        } else inspect(rt, ex, &b, 0, 1);
        rt->throwing = 0;
        copy_err(err, errmax, b.p, NULL);
        sb_free(&b);
        return 1;
    }
    return 0;
}
