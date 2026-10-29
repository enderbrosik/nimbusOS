/* NimbusOS JavaScript engine - standard library (Object, Function, Array, String, Number, Math, JSON, Error,
 * console, Date) and the public runtime API. */
#include "js/js_int.h"

#define ARG(i) ((i) < nargs ? args[i] : v_undef())
#define U v_undef()
#define RET_IF_BAD() do { if (NOVAL(rt)) return U; } while (0)

static jsval vnum(long long i) { return v_num(num_from_int(i)); }

jsobj *native_new(js_rt *rt, const char *name, js_native fn, int nargs, int magic)
{
    (void)nargs;
    jsobj *f = obj_new(rt, rt->proto_function, CL_FUNCTION);
    f->native = fn;
    f->magic = magic;
    prop_put(rt, f, rt->a_name, v_str(atom_c(rt, name)), PF_HIDDEN);
    return f;
}

void def_native(js_rt *rt, jsobj *o, const char *name, js_native fn, int nargs, int magic)
{
    prop_put(rt, o, atom_c(rt, name), v_obj(native_new(rt, name, fn, nargs, magic)), PF_HIDDEN);
}

void set_hidden(js_rt *rt, jsobj *o, const char *name, jsval v) { prop_put(rt, o, atom_c(rt, name), v, PF_HIDDEN); }

void def_accessor(js_rt *rt, jsobj *o, const char *name, js_native getter, js_native setter, int magic)
{
    jsval g = getter ? v_obj(native_new(rt, name, getter, 0, magic)) : v_undef();
    jsval st = setter ? v_obj(native_new(rt, name, setter, 1, magic)) : v_undef();
    put_accessor(rt, o, atom_c(rt, name), g, st, PF_HIDDEN);
}

/* ---- helpers ----------------------------------------------------------------------------------------------- */

static long long rel_index(js_rt *rt, jsval v, long long len, long long dflt)
{
    if (v.t == JT_UNDEF) return dflt;
    js_num n = to_num(rt, v);
    if (n == JS_NAN) return 0;
    if (n == JS_PINF) return len;
    if (n == JS_NINF) return 0;
    long long i = n / JS_SCALE;
    if (i < 0) { i += len; if (i < 0) i = 0; }
    else if (i > len) i = len;
    return i;
}

static jsobj *this_array(js_rt *rt, jsval thisv)
{
    if (thisv.t != JT_OBJ || thisv.u.o->cls != CL_ARRAY) {
        throw_error(rt, "TypeError", "Array method called on a value that is not an array", NULL, NULL);
        return NULL;
    }
    return thisv.u.o;
}

static jsval fn_arg(js_rt *rt, jsval v)
{
    if (!is_callable(v)) { throw_error(rt, "TypeError", "callback is not a function", NULL, NULL); return U; }
    return v;
}

/* ---- inspect (console.log formatting) ---------------------------------------------------------------------- */

static int is_ident(jstr *k)
{
    if (!k->len) return 0;
    for (unsigned i = 0; i < k->len; i++) {
        char c = k->s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (i && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

static int has_char(jstr *s, char c)
{
    for (unsigned i = 0; i < s->len; i++) if (s->s[i] == c) return 1;
    return 0;
}

static void quote_str(sbuf *b, jstr *s)
{
    char q = '\'';
    if (has_char(s, '\'')) q = has_char(s, '"') ? '`' : '"';
    sb_addc(b, q);
    for (unsigned i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (c == '\n') sb_adds(b, "\\n");
        else if (c == '\t') sb_adds(b, "\\t");
        else if (c == '\r') sb_adds(b, "\\r");
        else if (c == '\\') sb_adds(b, "\\\\");
        else sb_addc(b, c);
    }
    sb_addc(b, q);
}

void inspect(js_rt *rt, jsval v, sbuf *b, int depth, int top)
{
    char buf[40];
    switch (v.t) {
    case JT_UNDEF: sb_adds(b, "undefined"); return;
    case JT_NULL: sb_adds(b, "null"); return;
    case JT_BOOL: sb_adds(b, v.u.b ? "true" : "false"); return;
    case JT_NUM: { int n = num_fmt(v.u.n, buf); sb_add(b, buf, (unsigned)n); return; }
    case JT_STR:
        if (top) sb_add(b, v.u.s->s, v.u.s->len); else quote_str(b, v.u.s);
        return;
    }
    jsobj *o = v.u.o;
    if (o->cls == CL_FUNCTION) {
        jsval nm = get_prop(rt, v, rt->a_name);
        if (nm.t == JT_STR && nm.u.s->len) { sb_adds(b, "[Function: "); sb_add(b, nm.u.s->s, nm.u.s->len); sb_addc(b, ']'); }
        else sb_adds(b, "[Function (anonymous)]");
        return;
    }
    if (o->cls == CL_ERROR) {
        jsval nm = get_prop(rt, v, rt->a_name), msg = get_prop(rt, v, rt->a_message);
        if (!top) sb_addc(b, '[');
        jstr *ns = to_str(rt, nm), *ms = to_str(rt, msg);
        sb_add(b, ns->s, ns->len);
        if (ms->len) { sb_adds(b, ": "); sb_add(b, ms->s, ms->len); }
        if (!top) sb_addc(b, ']');
        return;
    }
    if (o->cls == CL_DATE) { jsval f = get_prop(rt, v, atom_c(rt, "toISOString")); jsval r = call_fn(rt, f, v, NULL, 0); if (r.t == JT_STR) sb_add(b, r.u.s->s, r.u.s->len); return; }
    if (depth > 2) { sb_adds(b, o->cls == CL_ARRAY ? "[Array]" : "[Object]"); return; }
    if (o->cls == CL_ARRAY) {
        if (!o->alen) { sb_adds(b, "[]"); return; }
        sb_adds(b, "[ ");
        for (unsigned i = 0; i < o->alen; i++) {
            if (i) sb_adds(b, ", ");
            if (i == 100) { int n = num_fmt(num_from_int(o->alen - 100), buf); sb_adds(b, "... "); sb_add(b, buf, (unsigned)n); sb_adds(b, " more items"); break; }
            inspect(rt, o->items[i], b, depth + 1, 0);
        }
        sb_adds(b, " ]");
        return;
    }
    int shown = 0;
    for (int i = 0; i < o->np; i++) if (!(o->props[i].flags & PF_HIDDEN)) shown++;
    if (!shown) { sb_adds(b, "{}"); return; }
    sb_adds(b, "{ ");
    int first = 1;
    for (int i = 0; i < o->np; i++) {
        if (o->props[i].flags & PF_HIDDEN) continue;
        if (!first) sb_adds(b, ", ");
        first = 0;
        jstr *k = o->props[i].key;
        if (is_ident(k)) sb_add(b, k->s, k->len); else quote_str(b, k);
        sb_adds(b, ": ");
        if (o->props[i].flags & PF_ACCESSOR) {
            jsobj *h = o->props[i].val.u.o;
            sb_adds(b, is_callable(h->items[0]) ? (is_callable(h->items[1]) ? "[Getter/Setter]" : "[Getter]") : "[Setter]");
        } else inspect(rt, o->props[i].val, b, depth + 1, 0);
    }
    sb_adds(b, " }");
}

/* ---- console ----------------------------------------------------------------------------------------------- */

static jsval n_console_log(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    sbuf b; sb_init(&b);
    for (int i = 0; i < nargs; i++) {
        if (i) sb_addc(&b, ' ');
        inspect(rt, args[i], &b, 0, 1);
    }
    sb_addc(&b, '\n');
    if (rt->out && !b.oom) rt->out(rt->out_ud, b.p, (int)b.len);
    sb_free(&b);
    return U;
}

/* ---- Object ------------------------------------------------------------------------------------------------ */

static jsval n_Object(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    if (v.t == JT_OBJ) return v;
    return v_obj(obj_new(rt, rt->proto_object, CL_OBJECT));
}

static jsobj *obj_arg(js_rt *rt, jsval v, const char *what)
{
    if (v.t != JT_OBJ) { throw_error(rt, "TypeError", what, NULL, NULL); return NULL; }
    return v.u.o;
}

/* magic: 0 keys, 1 values, 2 entries, 3 getOwnPropertyNames */
static jsval n_Object_keys(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    int mode = rt->cur_magic;
    jsval v = ARG(0);
    if (v.t == JT_STR) { jsobj *a = arr_new(rt, 0); char buf[16]; for (unsigned i = 0; i < v.u.s->len; i++) { int n = num_fmt(num_from_int(i), buf); arr_push(rt, a, mode == 0 ? v_str(str_new(rt, buf, (unsigned)n)) : v_str(str_new(rt, v.u.s->s + i, 1))); } return v_obj(a); }
    jsobj *o = obj_arg(rt, v, "Cannot convert undefined or null to object");
    if (!o) return U;
    jsobj *keys = own_keys(rt, o, mode == 3);
    if (mode == 0 || mode == 3) return v_obj(keys);
    jsobj *out = arr_new(rt, 0);
    for (unsigned i = 0; i < keys->alen; i++) {
        jsval val = get_prop(rt, v, to_atom(rt, keys->items[i].u.s));
        RET_IF_BAD();
        if (mode == 1) arr_push(rt, out, val);
        else { jsobj *pair = arr_new(rt, 0); arr_push(rt, pair, keys->items[i]); arr_push(rt, pair, val); arr_push(rt, out, v_obj(pair)); }
    }
    return v_obj(out);
}

static jsval n_Object_assign(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *t = obj_arg(rt, ARG(0), "Cannot convert undefined or null to object");
    if (!t) return U;
    for (int i = 1; i < nargs; i++) {
        if (args[i].t != JT_OBJ) continue;
        jsobj *keys = own_keys(rt, args[i].u.o, 0);
        for (unsigned k = 0; k < keys->alen; k++) {
            jstr *key = to_atom(rt, keys->items[k].u.s);
            jsval v = get_prop(rt, args[i], key); RET_IF_BAD();
            set_prop(rt, v_obj(t), key, v); RET_IF_BAD();
        }
    }
    return v_obj(t);
}

static jsval n_Object_create(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval p = ARG(0);
    if (p.t != JT_OBJ && p.t != JT_NULL) return throw_error(rt, "TypeError", "Object prototype may only be an Object or null", NULL, NULL);
    return v_obj(obj_new(rt, p.t == JT_OBJ ? p.u.o : NULL, CL_OBJECT));
}

static jsval n_Object_getProto(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    if (v.t == JT_STR) return v_obj(rt->proto_string);
    if (v.t == JT_NUM) return v_obj(rt->proto_number);
    if (v.t != JT_OBJ) return v.t == JT_BOOL ? v_obj(rt->proto_boolean) : throw_error(rt, "TypeError", "Cannot convert undefined or null to object", NULL, NULL);
    return v.u.o->proto ? v_obj(v.u.o->proto) : v_null();
}

static jsval n_Object_setProto(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval o = ARG(0), p = ARG(1);
    if (o.t == JT_OBJ && (p.t == JT_OBJ || p.t == JT_NULL)) o.u.o->proto = p.t == JT_OBJ ? p.u.o : NULL;
    return o;
}

static jsval n_Object_freeze(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    if (v.t == JT_OBJ) {
        v.u.o->extensible = 0;
        for (int i = 0; i < v.u.o->np; i++) v.u.o->props[i].flags |= PF_CONST;
        if (v.u.o->cls == CL_ARRAY) v.u.o->extensible = 0;
    }
    return v;
}

static jsval n_Object_defineProperty(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *o = obj_arg(rt, ARG(0), "Object.defineProperty called on non-object");
    if (!o) return U;
    jstr *key = to_key_atom(rt, ARG(1)); RET_IF_BAD();
    jsval d = ARG(2);
    if (d.t != JT_OBJ) return throw_error(rt, "TypeError", "Property description must be an object", NULL, NULL);
    jsval val = get_prop(rt, d, atom_c(rt, "value")); RET_IF_BAD();
    jsval en = get_prop(rt, d, atom_c(rt, "enumerable")), wr = get_prop(rt, d, atom_c(rt, "writable"));
    int flags = (to_bool(en) ? 0 : PF_HIDDEN) | (to_bool(wr) ? 0 : PF_CONST);
    jsval gt = get_prop(rt, d, rt->a_get), st = get_prop(rt, d, rt->a_set); RET_IF_BAD();
    if (is_callable(gt) || is_callable(st)) { put_accessor(rt, o, key, is_callable(gt) ? gt : U, is_callable(st) ? st : U, to_bool(en) ? 0 : PF_HIDDEN); return ARG(0); }
    if (o->cls == CL_ARRAY) { set_prop(rt, ARG(0), key, val); return ARG(0); }
    prop *p = prop_find(o, key);
    if (p) { p->val = val; p->flags = (unsigned char)flags; } else prop_put(rt, o, key, val, flags);
    return ARG(0);
}

static jsval n_hasOwn(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval t = rt->cur_magic == 1 ? ARG(0) : thisv;
    jsval k = rt->cur_magic == 1 ? ARG(1) : ARG(0);
    jstr *key = to_key_atom(rt, k); RET_IF_BAD();
    if (t.t == JT_STR) { unsigned i; return v_bool(key == rt->a_length || (atom_index(key, &i) && i < t.u.s->len)); }
    if (t.t != JT_OBJ) return v_bool(0);
    unsigned idx;
    if (t.u.o->cls == CL_ARRAY && (key == rt->a_length || (atom_index(key, &idx) && idx < t.u.o->alen))) return v_bool(1);
    return v_bool(prop_find(t.u.o, key) != NULL);
}

static jsval n_obj_toString(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    const char *s = "[object Object]";
    if (thisv.t == JT_UNDEF) s = "[object Undefined]";
    else if (thisv.t == JT_NULL) s = "[object Null]";
    else if (thisv.t == JT_NUM) s = "[object Number]";
    else if (thisv.t == JT_STR) s = "[object String]";
    else if (thisv.t == JT_BOOL) s = "[object Boolean]";
    else if (thisv.u.o->cls == CL_ARRAY) s = "[object Array]";
    else if (thisv.u.o->cls == CL_FUNCTION) s = "[object Function]";
    else if (thisv.u.o->cls == CL_ERROR) s = "[object Error]";
    else if (thisv.u.o->cls == CL_DATE) s = "[object Date]";
    return v_str(atom_c(rt, s));
}

static jsval n_obj_valueOf(js_rt *rt, jsval thisv, jsval *args, int nargs) { return thisv; }

static jsval n_isPrototypeOf(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    if (thisv.t != JT_OBJ || v.t != JT_OBJ) return v_bool(0);
    for (jsobj *o = v.u.o->proto; o; o = o->proto) if (o == thisv.u.o) return v_bool(1);
    return v_bool(0);
}

/* ---- Function ---------------------------------------------------------------------------------------------- */

static jsval n_Function(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    return throw_error(rt, "EvalError", "Function constructor is not supported", NULL, NULL);
}

static jsval n_fn_call(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (!is_callable(thisv)) return throw_error(rt, "TypeError", "Function.prototype.call called on non-function", NULL, NULL);
    return call_fn(rt, thisv, ARG(0), nargs > 1 ? args + 1 : NULL, nargs > 1 ? nargs - 1 : 0);
}

static jsval n_fn_apply(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (!is_callable(thisv)) return throw_error(rt, "TypeError", "Function.prototype.apply called on non-function", NULL, NULL);
    jsval a = ARG(1);
    if (a.t == JT_UNDEF || a.t == JT_NULL) return call_fn(rt, thisv, ARG(0), NULL, 0);
    if (a.t != JT_OBJ || a.u.o->cls != CL_ARRAY) return throw_error(rt, "TypeError", "CreateListFromArrayLike called on non-object", NULL, NULL);
    return call_fn(rt, thisv, ARG(0), a.u.o->items, (int)a.u.o->alen);
}

static jsval n_fn_bind(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (!is_callable(thisv)) return throw_error(rt, "TypeError", "Bind must be called on a function", NULL, NULL);
    jsobj *b = obj_new(rt, rt->proto_function, CL_FUNCTION);
    b->bound_fn = thisv.u.o;
    b->bound_this = ARG(0);
    b->nbound = nargs > 1 ? nargs - 1 : 0;
    if (b->nbound) {
        b->bound_args = (jsval *)JS_MALLOC((size_t)b->nbound * sizeof(jsval));
        if (!b->bound_args) { b->nbound = 0; rt->abort_msg = "out of memory"; return U; }
        for (int i = 0; i < b->nbound; i++) b->bound_args[i] = args[i + 1];
    }
    return v_obj(b);
}

static jsval n_fn_toString(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    sbuf b; sb_init(&b);
    sb_adds(&b, "function ");
    jsval nm = get_prop(rt, thisv, rt->a_name);
    if (nm.t == JT_STR) sb_add(&b, nm.u.s->s, nm.u.s->len);
    sb_adds(&b, "() { [native code] }");
    jstr *s = sb_str(rt, &b);
    sb_free(&b);
    return v_str(s);
}

/* ---- Array ------------------------------------------------------------------------------------------------- */

static jsval n_Array(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = arr_new(rt, 0);
    if (nargs == 1 && args[0].t == JT_NUM) {
        js_num n = args[0].u.n;
        if (!num_isfin(n) || n < 0 || n % JS_SCALE || n / JS_SCALE > 0x1000000) return throw_error(rt, "RangeError", "Invalid array length", NULL, NULL);
        arr_set_len(rt, a, (unsigned)(n / JS_SCALE));
        return v_obj(a);
    }
    for (int i = 0; i < nargs; i++) arr_push(rt, a, args[i]);
    return v_obj(a);
}

static jsval n_Array_isArray(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    return v_bool(v.t == JT_OBJ && v.u.o->cls == CL_ARRAY);
}

static jsval n_Array_from(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval src = ARG(0), mapf = ARG(1);
    jsobj *out = arr_new(rt, 0);
    if (src.t == JT_OBJ && src.u.o->cls == CL_ARRAY) { for (unsigned i = 0; i < src.u.o->alen; i++) arr_push(rt, out, src.u.o->items[i]); }
    else if (src.t == JT_STR) {
        const char *s = src.u.s->s; unsigned n = src.u.s->len;
        for (unsigned i = 0; i < n; ) { unsigned k = 1; unsigned char c = (unsigned char)s[i]; if (c >= 0xF0) k = 4; else if (c >= 0xE0) k = 3; else if (c >= 0xC0) k = 2; if (i + k > n) k = n - i; arr_push(rt, out, v_str(str_new(rt, s + i, k))); i += k; }
    } else if (src.t == JT_OBJ) {                              /* array-like: { length: n } */
        jsval ln = get_prop(rt, src, rt->a_length); RET_IF_BAD();
        js_num n = to_num(rt, ln);
        long long cnt = num_isfin(n) && n > 0 ? n / JS_SCALE : 0;
        if (cnt > 0x1000000) cnt = 0x1000000;
        for (long long i = 0; i < cnt; i++) { jsval v = get_member(rt, src, vnum(i)); RET_IF_BAD(); arr_push(rt, out, v); }
    } else if (src.t != JT_UNDEF && src.t != JT_NULL) { /* numbers, booleans: empty */ }
    else return throw_error(rt, "TypeError", "undefined is not iterable", NULL, NULL);
    if (is_callable(mapf))
        for (unsigned i = 0; i < out->alen; i++) {
            jsval a2[2] = { out->items[i], vnum(i) };
            jsval r = call_fn(rt, mapf, U, a2, 2); RET_IF_BAD();
            out->items[i] = r;
        }
    return v_obj(out);
}

static jsval n_Array_of(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = arr_new(rt, 0);
    for (int i = 0; i < nargs; i++) arr_push(rt, a, args[i]);
    return v_obj(a);
}

static jsval n_arr_push(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    if (!a->extensible) return throw_error(rt, "TypeError", "Cannot add property, object is not extensible", NULL, NULL);
    for (int i = 0; i < nargs; i++) arr_push(rt, a, args[i]);
    return vnum(a->alen);
}

static jsval n_arr_pop(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    if (!a->alen) return U;
    jsval v = a->items[a->alen - 1];
    a->alen--;
    return v;
}

static jsval n_arr_shift(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    if (!a->alen) return U;
    jsval v = a->items[0];
    memmove(a->items, a->items + 1, (size_t)(a->alen - 1) * sizeof(jsval));
    a->alen--;
    return v;
}

static jsval n_arr_unshift(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    unsigned old = a->alen;
    arr_set_len(rt, a, old + (unsigned)nargs);
    if (a->alen != old + (unsigned)nargs) return U;
    memmove(a->items + nargs, a->items, (size_t)old * sizeof(jsval));
    for (int i = 0; i < nargs; i++) a->items[i] = args[i];
    return vnum(a->alen);
}

static jsval n_arr_slice(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    long long len = a->alen, s = rel_index(rt, ARG(0), len, 0), e = rel_index(rt, ARG(1), len, len);
    jsobj *out = arr_new(rt, 0);
    for (long long i = s; i < e && i < (long long)a->alen; i++) arr_push(rt, out, a->items[i]);
    return v_obj(out);
}

static jsval n_arr_splice(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    long long len = a->alen;
    long long start = rel_index(rt, ARG(0), len, 0);
    long long del = len - start;
    if (nargs >= 2) {
        js_num n = to_num(rt, args[1]);
        del = !num_isfin(n) ? (n == JS_PINF ? len - start : 0) : n / JS_SCALE;
        if (del < 0) del = 0;
        if (del > len - start) del = len - start;
    } else if (nargs == 0) del = 0;
    int ins = nargs > 2 ? nargs - 2 : 0;
    jsobj *removed = arr_new(rt, 0);
    for (long long i = 0; i < del; i++) arr_push(rt, removed, a->items[start + i]);
    unsigned nlen = (unsigned)(len - del + ins);
    if (ins > del) {
        arr_set_len(rt, a, nlen);
        if (a->alen != nlen) return U;
        memmove(a->items + start + ins, a->items + start + del, (size_t)(len - start - del) * sizeof(jsval));
    } else {
        memmove(a->items + start + ins, a->items + start + del, (size_t)(len - start - del) * sizeof(jsval));
        a->alen = nlen;
    }
    for (int i = 0; i < ins; i++) a->items[start + i] = args[2 + i];
    return v_obj(removed);
}

static jsval n_arr_concat(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    jsobj *out = arr_new(rt, 0);
    for (unsigned i = 0; i < a->alen; i++) arr_push(rt, out, a->items[i]);
    for (int k = 0; k < nargs; k++) {
        if (args[k].t == JT_OBJ && args[k].u.o->cls == CL_ARRAY) { for (unsigned i = 0; i < args[k].u.o->alen; i++) arr_push(rt, out, args[k].u.o->items[i]); }
        else arr_push(rt, out, args[k]);
    }
    return v_obj(out);
}

static jsval n_arr_join(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    jstr *sep = ARG(0).t == JT_UNDEF ? atom_c(rt, ",") : to_str(rt, ARG(0));
    RET_IF_BAD();
    sbuf b; sb_init(&b);
    for (unsigned i = 0; i < a->alen; i++) {
        if (i) sb_add(&b, sep->s, sep->len);
        jsval v = a->items[i];
        if (v.t == JT_UNDEF || v.t == JT_NULL) continue;
        jstr *s = to_str(rt, v);
        if (NOVAL(rt)) { sb_free(&b); return U; }
        sb_add(&b, s->s, s->len);
    }
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return v_str(r);
}

static jsval n_arr_reverse(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    for (unsigned i = 0, j = a->alen; i + 1 < j; i++, j--) { jsval t = a->items[i]; a->items[i] = a->items[j - 1]; a->items[j - 1] = t; }
    return thisv;
}

static jsval n_arr_indexOf(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    int mode = rt->cur_magic;                                   /* 0 indexOf, 1 lastIndexOf, 2 includes */
    if (mode == 1) {
        for (long long i = (long long)a->alen - 1; i >= 0; i--) if (strict_eq(a->items[i], ARG(0))) return vnum(i);
        return vnum(-1);
    }
    long long from = rel_index(rt, ARG(1), a->alen, 0);
    for (long long i = from; i < (long long)a->alen; i++) {
        jsval v = a->items[i];
        if (mode == 2 ? (strict_eq(v, ARG(0)) || (v.t == JT_NUM && ARG(0).t == JT_NUM && v.u.n == JS_NAN && ARG(0).u.n == JS_NAN)) : strict_eq(v, ARG(0)))
            return mode == 2 ? v_bool(1) : vnum(i);
    }
    return mode == 2 ? v_bool(0) : vnum(-1);
}

/* magic: 0 forEach, 1 map, 2 filter, 3 some, 4 every, 5 find, 6 findIndex, 7 findLast, 8 findLastIndex */
static jsval n_arr_iter(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    int mode = rt->cur_magic;
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    jsval f = fn_arg(rt, ARG(0)); RET_IF_BAD();
    jsobj *out = (mode == 1 || mode == 2) ? arr_new(rt, 0) : NULL;
    int rev = mode >= 7;
    for (long long k = 0; k < (long long)a->alen; k++) {
        long long i = rev ? (long long)a->alen - 1 - k : k;
        if (i >= (long long)a->alen) break;
        jsval cb[3] = { a->items[i], vnum(i), thisv };
        jsval r = call_fn(rt, f, ARG(1), cb, 3);
        RET_IF_BAD();
        switch (mode) {
        case 1: arr_push(rt, out, r); break;
        case 2: if (to_bool(r)) arr_push(rt, out, cb[0]); break;
        case 3: if (to_bool(r)) return v_bool(1); break;
        case 4: if (!to_bool(r)) return v_bool(0); break;
        case 5: case 7: if (to_bool(r)) return cb[0]; break;
        case 6: case 8: if (to_bool(r)) return vnum(i); break;
        }
    }
    switch (mode) {
    case 1: case 2: return v_obj(out);
    case 3: return v_bool(0);
    case 4: return v_bool(1);
    case 6: case 8: return vnum(-1);
    default: return U;
    }
}

static jsval n_arr_reduce(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    int right = rt->cur_magic;
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    jsval f = fn_arg(rt, ARG(0)); RET_IF_BAD();
    long long n = a->alen, k = 0;
    jsval acc;
    if (nargs >= 2) acc = args[1];
    else {
        if (!n) return throw_error(rt, "TypeError", "Reduce of empty array with no initial value", NULL, NULL);
        acc = a->items[right ? n - 1 : 0];
        k = 1;
    }
    for (; k < n; k++) {
        long long i = right ? n - 1 - k : k;
        if (i >= (long long)a->alen) break;
        jsval cb[4] = { acc, a->items[i], vnum(i), thisv };
        acc = call_fn(rt, f, U, cb, 4);
        RET_IF_BAD();
    }
    return acc;
}

static int sort_cmp(js_rt *rt, jsval f, jsval x, jsval y, int *err)
{
    if (x.t == JT_UNDEF) return y.t == JT_UNDEF ? 0 : 1;
    if (y.t == JT_UNDEF) return -1;
    if (f.t != JT_UNDEF) {
        jsval cb[2] = { x, y };
        jsval r = call_fn(rt, f, U, cb, 2);
        if (NOVAL(rt)) { *err = 1; return 0; }
        js_num n = to_num(rt, r);
        return n < 0 ? -1 : n > 0 && n != JS_NAN ? 1 : 0;
    }
    jstr *a = to_str(rt, x), *b = to_str(rt, y);
    if (NOVAL(rt)) { *err = 1; return 0; }
    unsigned n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->s, b->s, n);
    if (c) return c < 0 ? -1 : 1;
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

static jsval n_arr_sort(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    jsval f = ARG(0);
    if (f.t != JT_UNDEF && !is_callable(f)) return throw_error(rt, "TypeError", "The comparison function must be either a function or undefined", NULL, NULL);
    unsigned n = a->alen;
    if (n < 2) return thisv;
    jsobj *tmpo = arr_new(rt, n);                             /* a heap array: the collector sees the values being merged */
    if (tmpo->alen != n) return U;
    jsval *tmp = tmpo->items;
    int err = 0;
    for (unsigned w = 1; w < n && !err; w *= 2) {                 /* bottom-up merge sort (stable) */
        for (unsigned lo = 0; lo < n && !err; lo += 2 * w) {
            unsigned mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            unsigned i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int c = sort_cmp(rt, f, a->items[i], a->items[j], &err);
                if (err) break;
                tmp[k++] = c <= 0 ? a->items[i++] : a->items[j++];
            }
            if (err) break;
            while (i < mid) tmp[k++] = a->items[i++];
            while (j < hi) tmp[k++] = a->items[j++];
        }
        if (!err) memcpy(a->items, tmp, (size_t)n * sizeof(jsval));
    }
    return err ? U : thisv;
}

static jsval n_arr_fill(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    long long len = a->alen, s = rel_index(rt, ARG(1), len, 0), e = rel_index(rt, ARG(2), len, len);
    for (long long i = s; i < e; i++) a->items[i] = ARG(0);
    return thisv;
}

static void flatten(js_rt *rt, jsobj *out, jsobj *a, long long depth)
{
    for (unsigned i = 0; i < a->alen; i++) {
        jsval v = a->items[i];
        if (v.t == JT_OBJ && v.u.o->cls == CL_ARRAY && depth > 0) flatten(rt, out, v.u.o, depth - 1);
        else arr_push(rt, out, v);
    }
}

static jsval n_arr_flat(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    long long d = ARG(0).t == JT_UNDEF ? 1 : num_to_int64(to_num(rt, ARG(0)));
    jsobj *out = arr_new(rt, 0);
    flatten(rt, out, a, d > 20 ? 20 : d);
    return v_obj(out);
}

static jsval n_arr_at(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsobj *a = this_array(rt, thisv); if (!a) return U;
    long long i = num_to_int64(to_num(rt, ARG(0)));
    if (i < 0) i += a->alen;
    return i >= 0 && i < (long long)a->alen ? a->items[i] : U;
}

/* ---- String ------------------------------------------------------------------------------------------------ */

static jsval n_String(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (!nargs) return v_str(rt->a_empty);
    return v_str(to_str(rt, args[0]));
}

static jstr *this_str(js_rt *rt, jsval thisv)
{
    if (thisv.t == JT_UNDEF || thisv.t == JT_NULL) { throw_error(rt, "TypeError", "String.prototype method called on null or undefined", NULL, NULL); return rt->a_empty; }
    return to_str(rt, thisv);
}

static jsval n_String_fromCharCode(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    sbuf b; sb_init(&b);
    for (int i = 0; i < nargs; i++) {
        unsigned cp = (unsigned)num_to_int32(to_num(rt, args[i])) & 0xFFFF;
        if (cp < 0x80) sb_addc(&b, (char)cp);
        else if (cp < 0x800) { sb_addc(&b, (char)(0xC0 | (cp >> 6))); sb_addc(&b, (char)(0x80 | (cp & 63))); }
        else { sb_addc(&b, (char)(0xE0 | (cp >> 12))); sb_addc(&b, (char)(0x80 | ((cp >> 6) & 63))); sb_addc(&b, (char)(0x80 | (cp & 63))); }
    }
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return v_str(r);
}

static jsval n_str_charAt(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    long long i = num_to_int64(to_num(rt, ARG(0)));
    if (rt->cur_magic == 2) {                                   /* at() */
        if (i < 0) i += s->len;
        return i >= 0 && i < (long long)s->len ? v_str(str_new(rt, s->s + i, 1)) : U;
    }
    if (i < 0 || i >= (long long)s->len) return rt->cur_magic == 0 ? v_str(rt->a_empty) : v_num(JS_NAN);
    if (rt->cur_magic == 1) {                                   /* charCodeAt: decode the UTF-8 character at i */
        unsigned char c = (unsigned char)s->s[i];
        if (c < 0x80) return vnum(c);
        unsigned cp = 0; int extra = 0;
        if (c >= 0xF0) { cp = c & 7; extra = 3; } else if (c >= 0xE0) { cp = c & 15; extra = 2; } else if (c >= 0xC0) { cp = c & 31; extra = 1; } else return vnum(c);
        for (int k = 1; k <= extra && i + k < (long long)s->len; k++) cp = (cp << 6) | ((unsigned char)s->s[i + k] & 63);
        return vnum(cp);
    }
    return v_str(str_new(rt, s->s + i, 1));
}

static long long find_sub(jstr *h, jstr *n, long long from)
{
    if (from < 0) from = 0;
    if (n->len == 0) return from <= (long long)h->len ? from : (long long)h->len;
    for (long long i = from; i + (long long)n->len <= (long long)h->len; i++)
        if (h->s[i] == n->s[0] && memcmp(h->s + i, n->s, n->len) == 0) return i;
    return -1;
}

static jsval n_str_indexOf(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    jstr *n = to_str(rt, ARG(0)); RET_IF_BAD();
    int mode = rt->cur_magic;                                   /* 0 indexOf, 1 lastIndexOf, 2 includes, 3 startsWith, 4 endsWith */
    if (mode == 1) {
        for (long long i = (long long)s->len - (long long)n->len; i >= 0; i--) if (memcmp(s->s + i, n->s, n->len) == 0) return vnum(i);
        return vnum(-1);
    }
    if (mode == 3) { long long p = rel_index(rt, ARG(1), s->len, 0); return v_bool(p + (long long)n->len <= (long long)s->len && memcmp(s->s + p, n->s, n->len) == 0); }
    if (mode == 4) { long long e = rel_index(rt, ARG(1), s->len, s->len); return v_bool(e >= (long long)n->len && memcmp(s->s + e - n->len, n->s, n->len) == 0); }
    long long r = find_sub(s, n, rel_index(rt, ARG(1), s->len, 0));
    return mode == 2 ? v_bool(r >= 0) : vnum(r);
}

static jsval n_str_slice(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    long long len = s->len, a, b;
    int mode = rt->cur_magic;                                   /* 0 slice, 1 substring, 2 substr */
    if (mode == 1) {
        js_num x = to_num(rt, ARG(0)), y = ARG(1).t == JT_UNDEF ? num_from_int(len) : to_num(rt, ARG(1));
        a = num_isfin(x) ? x / JS_SCALE : (x == JS_PINF ? len : 0); b = num_isfin(y) ? y / JS_SCALE : (y == JS_PINF ? len : 0);
        if (a < 0) a = 0;
        if (b < 0) b = 0;
        if (a > len) a = len;
        if (b > len) b = len;
        if (a > b) { long long t = a; a = b; b = t; }
    } else if (mode == 2) {
        a = rel_index(rt, ARG(0), len, 0);
        long long n = ARG(1).t == JT_UNDEF ? len - a : num_to_int64(to_num(rt, ARG(1)));
        if (n < 0) n = 0;
        b = a + n > len ? len : a + n;
    } else {
        a = rel_index(rt, ARG(0), len, 0); b = rel_index(rt, ARG(1), len, len);
    }
    if (a >= b) return v_str(rt->a_empty);
    return v_str(str_new(rt, s->s + a, (unsigned)(b - a)));
}

static jsval n_str_case(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    jstr *r = str_new(rt, s->s, s->len);
    for (unsigned i = 0; i < r->len; i++) {
        char c = r->s[i];
        if (rt->cur_magic == 0 && c >= 'a' && c <= 'z') r->s[i] = (char)(c - 32);
        else if (rt->cur_magic == 1 && c >= 'A' && c <= 'Z') r->s[i] = (char)(c + 32);
    }
    return v_str(r);
}

static jsval n_str_trim(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    unsigned a = 0, e = s->len;
    int mode = rt->cur_magic;                                   /* 0 both, 1 start, 2 end */
    while (mode != 2 && a < e && (s->s[a] == ' ' || s->s[a] == '\t' || s->s[a] == '\n' || s->s[a] == '\r' || s->s[a] == '\v' || s->s[a] == '\f')) a++;
    while (mode != 1 && e > a && (s->s[e - 1] == ' ' || s->s[e - 1] == '\t' || s->s[e - 1] == '\n' || s->s[e - 1] == '\r' || s->s[e - 1] == '\v' || s->s[e - 1] == '\f')) e--;
    return v_str(str_new(rt, s->s + a, e - a));
}

static jsval n_str_split(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    jsobj *out = arr_new(rt, 0);
    long long lim = ARG(1).t == JT_UNDEF ? 0x7FFFFFFF : num_to_int64(to_num(rt, ARG(1)));
    if (ARG(0).t == JT_UNDEF) { arr_push(rt, out, v_str(s)); return v_obj(out); }
    jstr *sep = to_str(rt, ARG(0)); RET_IF_BAD();
    if (!sep->len) {
        for (unsigned i = 0; i < s->len && (long long)out->alen < lim; ) {
            unsigned k = 1; unsigned char c = (unsigned char)s->s[i];
            if (c >= 0xF0) k = 4; else if (c >= 0xE0) k = 3; else if (c >= 0xC0) k = 2;
            if (i + k > s->len) k = s->len - i;
            arr_push(rt, out, v_str(str_new(rt, s->s + i, k)));
            i += k;
        }
        return v_obj(out);
    }
    long long pos = 0;
    for (;;) {
        long long f = find_sub(s, sep, pos);
        if ((long long)out->alen >= lim) break;
        if (f < 0) { arr_push(rt, out, v_str(str_new(rt, s->s + pos, (unsigned)(s->len - pos)))); break; }
        arr_push(rt, out, v_str(str_new(rt, s->s + pos, (unsigned)(f - pos))));
        pos = f + sep->len;
    }
    return v_obj(out);
}

/* replace / replaceAll with a string pattern; the replacement may be a string or a function */
static jsval n_str_replace(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    jstr *pat = to_str(rt, ARG(0)); RET_IF_BAD();
    jsval rep = ARG(1);
    jstr *reps = is_callable(rep) ? NULL : to_str(rt, rep); RET_IF_BAD();
    int all = rt->cur_magic;
    sbuf b; sb_init(&b);
    long long pos = 0, from = 0;
    for (;;) {
        long long f = find_sub(s, pat, from);
        if (f < 0) break;
        sb_add(&b, s->s + pos, (unsigned)(f - pos));
        if (reps) {                                             /* $& is the match; $$ a dollar */
            for (unsigned i = 0; i < reps->len; i++) {
                if (reps->s[i] == '$' && i + 1 < reps->len && reps->s[i + 1] == '&') { sb_add(&b, pat->s, pat->len); i++; }
                else if (reps->s[i] == '$' && i + 1 < reps->len && reps->s[i + 1] == '$') { sb_addc(&b, '$'); i++; }
                else sb_addc(&b, reps->s[i]);
            }
        } else {
            jsval cb[3] = { v_str(pat), vnum(f), v_str(s) };
            jsval r = call_fn(rt, rep, U, cb, 3);
            if (NOVAL(rt)) { sb_free(&b); return U; }
            jstr *rs = to_str(rt, r);
            sb_add(&b, rs->s, rs->len);
        }
        pos = f + pat->len;
        from = pat->len ? pos : pos + 1;
        if (!all || from > (long long)s->len) break;
    }
    if (pos <= (long long)s->len) sb_add(&b, s->s + pos, (unsigned)(s->len - pos));
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return v_str(r);
}

static jsval n_str_repeat(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    js_num n = to_num(rt, ARG(0));
    if (!num_isfin(n) || n < 0) return throw_error(rt, "RangeError", "Invalid count value", NULL, NULL);
    long long c = n / JS_SCALE;
    if (c * (long long)s->len > 0x1000000) return throw_error(rt, "RangeError", "Invalid string length", NULL, NULL);
    sbuf b; sb_init(&b);
    for (long long i = 0; i < c; i++) sb_add(&b, s->s, s->len);
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return v_str(r);
}

static jsval n_str_pad(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    long long target = num_to_int64(to_num(rt, ARG(0)));
    jstr *pad = ARG(1).t == JT_UNDEF ? atom_c(rt, " ") : to_str(rt, ARG(1));
    if (target <= (long long)s->len || !pad->len) return v_str(s);
    if (target > 0x1000000) return throw_error(rt, "RangeError", "Invalid string length", NULL, NULL);
    sbuf b; sb_init(&b);
    long long fill = target - s->len;
    if (rt->cur_magic == 1) sb_add(&b, s->s, s->len);          /* padEnd */
    for (long long i = 0; i < fill; i++) sb_addc(&b, pad->s[i % pad->len]);
    if (rt->cur_magic == 0) sb_add(&b, s->s, s->len);          /* padStart */
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return v_str(r);
}

static jsval n_str_concat(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = this_str(rt, thisv); RET_IF_BAD();
    for (int i = 0; i < nargs; i++) { jstr *t = to_str(rt, args[i]); RET_IF_BAD(); s = str_cat(rt, s, t); }
    return v_str(s);
}

static jsval n_str_localeCompare(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *a = this_str(rt, thisv); RET_IF_BAD();
    jstr *b = to_str(rt, ARG(0)); RET_IF_BAD();
    unsigned n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->s, b->s, n);
    if (!c) c = a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
    return vnum(c < 0 ? -1 : c > 0 ? 1 : 0);
}

static jsval n_str_toString(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_STR) return throw_error(rt, "TypeError", "String.prototype.toString requires a string", NULL, NULL);
    return thisv;
}

/* ---- Number and Boolean ------------------------------------------------------------------------------------ */

static jsval n_Number(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (!nargs) return v_num(0);
    return v_num(to_num(rt, args[0]));
}

static jsval n_Boolean(js_rt *rt, jsval thisv, jsval *args, int nargs) { return v_bool(to_bool(ARG(0))); }

static jsval n_isNaN(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    if (rt->cur_magic == 1 && v.t != JT_NUM) return v_bool(0);        /* Number.isNaN */
    js_num n = to_num(rt, v); RET_IF_BAD();
    return v_bool(n == JS_NAN);
}

static jsval n_isFinite(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    if (rt->cur_magic == 1 && v.t != JT_NUM) return v_bool(0);        /* Number.isFinite */
    js_num n = to_num(rt, v); RET_IF_BAD();
    return v_bool(num_isfin(n));
}

static jsval n_Number_isInteger(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jsval v = ARG(0);
    return v_bool(v.t == JT_NUM && num_isfin(v.u.n) && v.u.n % JS_SCALE == 0);
}

static jsval n_parseFloat(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = to_str(rt, ARG(0)); RET_IF_BAD();
    int used;
    js_num n = num_parse_prefix(s->s, (int)s->len, &used);
    return v_num(used ? n : JS_NAN);
}

static jsval n_parseInt(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = to_str(rt, ARG(0)); RET_IF_BAD();
    int radix = ARG(1).t == JT_UNDEF ? 0 : num_to_int32(to_num(rt, ARG(1)));
    unsigned i = 0;
    while (i < s->len && (s->s[i] == ' ' || s->s[i] == '\t' || s->s[i] == '\n' || s->s[i] == '\r')) i++;
    int neg = 0;
    if (i < s->len && (s->s[i] == '+' || s->s[i] == '-')) { neg = s->s[i] == '-'; i++; }
    if ((radix == 0 || radix == 16) && i + 1 < s->len && s->s[i] == '0' && (s->s[i + 1] == 'x' || s->s[i + 1] == 'X')) { i += 2; radix = 16; }
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) return v_num(JS_NAN);
    long long v = 0; int nd = 0;
    for (; i < s->len; i++) {
        char c = s->s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= radix) break;
        if (v < 0x7FFFFFFFFFFFLL) v = v * radix + d;
        nd++;
    }
    if (!nd) return v_num(JS_NAN);
    return v_num(num_from_int(neg ? -v : v));
}

static jsval n_num_toString(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_NUM) return throw_error(rt, "TypeError", "Number.prototype.toString requires a number", NULL, NULL);
    char buf[80];
    int radix = ARG(0).t == JT_UNDEF ? 10 : num_to_int32(to_num(rt, ARG(0)));
    if (radix < 2 || radix > 36) return throw_error(rt, "RangeError", "toString() radix must be between 2 and 36", NULL, NULL);
    int n = num_fmt_radix(thisv.u.n, radix, buf);
    return v_str(str_new(rt, buf, (unsigned)n));
}

static jsval n_num_toFixed(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_NUM) return throw_error(rt, "TypeError", "Number.prototype.toFixed requires a number", NULL, NULL);
    char buf[80];
    int d = num_to_int32(to_num(rt, ARG(0)));
    if (d < 0 || d > 100) return throw_error(rt, "RangeError", "toFixed() digits argument must be between 0 and 100", NULL, NULL);
    int n = num_fmt_fixed(thisv.u.n, d, buf);
    return v_str(str_new(rt, buf, (unsigned)n));
}

static jsval n_num_valueOf(js_rt *rt, jsval thisv, jsval *args, int nargs) { return thisv; }

static jsval n_bool_toString(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_BOOL) return throw_error(rt, "TypeError", "Boolean.prototype.toString requires a boolean", NULL, NULL);
    return v_str(thisv.u.b ? rt->a_true : rt->a_false);
}

/* ---- Math -------------------------------------------------------------------------------------------------- */

static jsval n_Math(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    int m = rt->cur_magic;
    js_num x = to_num(rt, ARG(0)); RET_IF_BAD();
    switch (m) {
    case 0: return v_num(num_floor(x));
    case 1: return v_num(num_ceil(x));
    case 2: return v_num(num_round(x));
    case 3: return v_num(num_trunc(x));
    case 4: return v_num(x == JS_NAN ? x : x < 0 ? num_neg(x) : x);
    case 5: return v_num(num_sqrt(x));
    case 6: return v_num(x == JS_NAN ? x : x > 0 ? JS_SCALE : x < 0 ? -JS_SCALE : 0);
    case 7: {                                                   /* pow */
        js_num y = to_num(rt, ARG(1)); RET_IF_BAD();
        return v_num(num_pow(x, y));
    }
    case 8: case 9: {                                           /* min, max */
        js_num r = m == 8 ? JS_PINF : JS_NINF;
        for (int i = 0; i < nargs; i++) {
            js_num v = to_num(rt, args[i]); RET_IF_BAD();
            if (v == JS_NAN) return v_num(JS_NAN);
            if (m == 8 ? v < r : v > r) r = v;
        }
        return v_num(r);
    }
    case 10: {                                                  /* random: [0, 1) with 4 decimals */
        rt->rng ^= rt->rng >> 12; rt->rng ^= rt->rng << 25; rt->rng ^= rt->rng >> 27;
        unsigned long long r = rt->rng * 0x2545F4914F6CDD1DULL;
        return v_num((js_num)((r >> 33) % (unsigned long long)JS_SCALE));
    }
    case 11: {                                                  /* hypot of two */
        js_num y = to_num(rt, ARG(1)); RET_IF_BAD();
        return v_num(num_sqrt(num_add(num_mul(x, x), num_mul(y, y))));
    }
    }
    return U;
}

/* ---- JSON -------------------------------------------------------------------------------------------------- */

jstr *json_quote(js_rt *rt, jstr *s)
{
    sbuf b; sb_init(&b);
    sb_addc(&b, '"');
    for (unsigned i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        switch (c) {
        case '"': sb_adds(&b, "\\\""); break;
        case '\\': sb_adds(&b, "\\\\"); break;
        case '\n': sb_adds(&b, "\\n"); break;
        case '\r': sb_adds(&b, "\\r"); break;
        case '\t': sb_adds(&b, "\\t"); break;
        case '\b': sb_adds(&b, "\\b"); break;
        case '\f': sb_adds(&b, "\\f"); break;
        default:
            if (c < 0x20) { static const char hx[] = "0123456789abcdef"; sb_adds(&b, "\\u00"); sb_addc(&b, hx[c >> 4]); sb_addc(&b, hx[c & 15]); }
            else sb_addc(&b, (char)c);
        }
    }
    sb_addc(&b, '"');
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return r;
}

static void indent_to(sbuf *b, jstr *ind, int level)
{
    sb_addc(b, '\n');
    for (int i = 0; i < level; i++) sb_add(b, ind->s, ind->len);
}

/* Returns 0 if the value is skipped (undefined, functions), 1 if written. */
static int json_write(js_rt *rt, jsval v, sbuf *b, jstr *ind, int level)
{
    if (level > 100) { throw_error(rt, "TypeError", "Converting circular structure to JSON", NULL, NULL); return 0; }
    if (v.t == JT_OBJ) {
        jsval tj = get_prop(rt, v, atom_c(rt, "toJSON"));
        if (NOVAL(rt)) return 0;
        if (is_callable(tj)) { v = call_fn(rt, tj, v, NULL, 0); if (NOVAL(rt)) return 0; }
    }
    char buf[40];
    switch (v.t) {
    case JT_UNDEF: return 0;
    case JT_NULL: sb_adds(b, "null"); return 1;
    case JT_BOOL: sb_adds(b, v.u.b ? "true" : "false"); return 1;
    case JT_NUM: if (num_isfin(v.u.n)) { int n = num_fmt(v.u.n, buf); sb_add(b, buf, (unsigned)n); } else sb_adds(b, "null"); return 1;
    case JT_STR: { jstr *q = json_quote(rt, v.u.s); sb_add(b, q->s, q->len); return 1; }
    }
    jsobj *o = v.u.o;
    if (o->cls == CL_FUNCTION) return 0;
    if (o->cls == CL_ARRAY) {
        if (!o->alen) { sb_adds(b, "[]"); return 1; }
        sb_addc(b, '[');
        for (unsigned i = 0; i < o->alen; i++) {
            if (i) sb_addc(b, ',');
            if (ind) indent_to(b, ind, level + 1);
            if (!json_write(rt, o->items[i], b, ind, level + 1)) { if (NOVAL(rt)) return 0; sb_adds(b, "null"); }
        }
        if (ind) indent_to(b, ind, level);
        sb_addc(b, ']');
        return 1;
    }
    int first = 1;
    sb_addc(b, '{');
    for (int i = 0; i < o->np; i++) {
        if (o->props[i].flags & PF_HIDDEN) continue;
        sbuf tmp; sb_init(&tmp);
        jsval pv = (o->props[i].flags & PF_ACCESSOR) ? get_prop(rt, v, o->props[i].key) : o->props[i].val;
        int ok = NOVAL(rt) ? 0 : json_write(rt, pv, &tmp, ind, level + 1);
        if (NOVAL(rt)) { sb_free(&tmp); return 0; }
        if (ok) {
            if (!first) sb_addc(b, ',');
            first = 0;
            if (ind) indent_to(b, ind, level + 1);
            jstr *q = json_quote(rt, o->props[i].key);
            sb_add(b, q->s, q->len);
            sb_addc(b, ':');
            if (ind) sb_addc(b, ' ');
            sb_add(b, tmp.p, tmp.len);
        }
        sb_free(&tmp);
    }
    if (!first && ind) indent_to(b, ind, level);
    sb_addc(b, '}');
    return 1;
}

static jsval n_JSON_stringify(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *ind = NULL;
    jsval sp = ARG(2);
    if (sp.t == JT_NUM) { int n = num_to_int32(sp.u.n); if (n > 10) n = 10; if (n > 0) { sbuf t; sb_init(&t); for (int i = 0; i < n; i++) sb_addc(&t, ' '); ind = sb_str(rt, &t); sb_free(&t); } }
    else if (sp.t == JT_STR && sp.u.s->len) ind = sp.u.s;
    sbuf b; sb_init(&b);
    int ok = json_write(rt, ARG(0), &b, ind, 0);
    if (NOVAL(rt)) { sb_free(&b); return U; }
    jsval r = ok ? v_str(sb_str(rt, &b)) : U;
    sb_free(&b);
    return r;
}

typedef struct { js_rt *rt; const char *s; int len, pos, depth; } jparse;

static void jws(jparse *p) { while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' || p->s[p->pos] == '\n' || p->s[p->pos] == '\r')) p->pos++; }

static void jerr(jparse *p)
{
    sbuf b; sb_init(&b);
    if (p->pos >= p->len) sb_adds(&b, "Unexpected end of JSON input");
    else { sb_adds(&b, "Unexpected token '"); sb_addc(&b, p->s[p->pos]); sb_adds(&b, "' in JSON at position "); char t[16]; int n = num_fmt(num_from_int(p->pos), t); sb_add(&b, t, (unsigned)n); }
    throw_error(p->rt, "SyntaxError", b.p, NULL, NULL);
    sb_free(&b);
}

static jsval jvalue(jparse *p);

static jstr *jstring(jparse *p)
{
    js_rt *rt = p->rt;
    sbuf b; sb_init(&b);
    p->pos++;
    while (p->pos < p->len && p->s[p->pos] != '"') {
        char c = p->s[p->pos++];
        if ((unsigned char)c < 0x20) { p->pos--; jerr(p); sb_free(&b); return rt->a_empty; }
        if (c != '\\') { sb_addc(&b, c); continue; }
        if (p->pos >= p->len) break;
        c = p->s[p->pos++];
        switch (c) {
        case 'n': sb_addc(&b, '\n'); break;
        case 't': sb_addc(&b, '\t'); break;
        case 'r': sb_addc(&b, '\r'); break;
        case 'b': sb_addc(&b, '\b'); break;
        case 'f': sb_addc(&b, '\f'); break;
        case '/': case '\\': case '"': sb_addc(&b, c); break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4; k++) {
                char h = p->pos < p->len ? p->s[p->pos] : 0;
                int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                if (d < 0) { jerr(p); sb_free(&b); return rt->a_empty; }
                cp = cp * 16 + (unsigned)d; p->pos++;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && p->pos + 5 < p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                unsigned lo = 0; int ok = 1;
                for (int k = 0; k < 4; k++) { char h = p->s[p->pos + 2 + k]; int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1; if (d < 0) { ok = 0; break; } lo = lo * 16 + (unsigned)d; }
                if (ok && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p->pos += 6; }
            }
            if (cp < 0x80) sb_addc(&b, (char)cp);
            else if (cp < 0x800) { sb_addc(&b, (char)(0xC0 | (cp >> 6))); sb_addc(&b, (char)(0x80 | (cp & 63))); }
            else if (cp < 0x10000) { sb_addc(&b, (char)(0xE0 | (cp >> 12))); sb_addc(&b, (char)(0x80 | ((cp >> 6) & 63))); sb_addc(&b, (char)(0x80 | (cp & 63))); }
            else { sb_addc(&b, (char)(0xF0 | (cp >> 18))); sb_addc(&b, (char)(0x80 | ((cp >> 12) & 63))); sb_addc(&b, (char)(0x80 | ((cp >> 6) & 63))); sb_addc(&b, (char)(0x80 | (cp & 63))); }
            break;
        }
        default: p->pos--; jerr(p); sb_free(&b); return rt->a_empty;
        }
    }
    if (p->pos >= p->len) { jerr(p); sb_free(&b); return rt->a_empty; }
    p->pos++;
    jstr *r = sb_str(rt, &b);
    sb_free(&b);
    return r;
}

static jsval jvalue(jparse *p)
{
    js_rt *rt = p->rt;
    jws(p);
    if (p->pos >= p->len) { jerr(p); return U; }
    if (++p->depth > 200) { throw_error(rt, "RangeError", "JSON nested too deeply", NULL, NULL); return U; }
    char c = p->s[p->pos];
    jsval r = U;
    if (c == '{') {
        jsobj *o = obj_new(rt, rt->proto_object, CL_OBJECT);
        p->pos++; jws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') p->pos++;
        else for (;;) {
            jws(p);
            if (p->pos >= p->len || p->s[p->pos] != '"') { jerr(p); p->depth--; return U; }
            jstr *k = to_atom(rt, jstring(p));
            if (NOVAL(rt)) { p->depth--; return U; }
            jws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') { jerr(p); p->depth--; return U; }
            p->pos++;
            jsval v = jvalue(p);
            if (NOVAL(rt)) { p->depth--; return U; }
            prop_put(rt, o, k, v, 0);
            jws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; break; }
            jerr(p); p->depth--; return U;
        }
        r = v_obj(o);
    } else if (c == '[') {
        jsobj *a = arr_new(rt, 0);
        p->pos++; jws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') p->pos++;
        else for (;;) {
            jsval v = jvalue(p);
            if (NOVAL(rt)) { p->depth--; return U; }
            arr_push(rt, a, v);
            jws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; break; }
            jerr(p); p->depth--; return U;
        }
        r = v_obj(a);
    } else if (c == '"') r = v_str(jstring(p));
    else if (c == 't' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) { p->pos += 4; r = v_bool(1); }
    else if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; r = v_bool(0); }
    else if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0) { p->pos += 4; r = v_null(); }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        int st = p->pos;
        if (p->s[p->pos] == '-') p->pos++;
        while (p->pos < p->len && ((p->s[p->pos] >= '0' && p->s[p->pos] <= '9') || p->s[p->pos] == '.' || p->s[p->pos] == 'e' || p->s[p->pos] == 'E' || p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        int used;
        js_num n = num_parse_prefix(p->s + st, p->pos - st, &used);
        if (!used || used != p->pos - st) { p->pos = st; jerr(p); p->depth--; return U; }
        r = v_num(n);
    } else { jerr(p); p->depth--; return U; }
    p->depth--;
    return r;
}

static jsval n_JSON_parse(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    jstr *s = to_str(rt, ARG(0)); RET_IF_BAD();
    jparse p = { rt, s->s, (int)s->len, 0, 0 };
    jsval v = jvalue(&p);
    RET_IF_BAD();
    jws(&p);
    if (p.pos < p.len) { jerr(&p); return U; }
    return v;
}

/* ---- Error ------------------------------------------------------------------------------------------------- */

static jsval n_Error(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    /* magic indexes the error kind: the prototype is found through the constructor stored at that global name */
    static const char *kinds[] = { "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError" };
    const char *kind = kinds[rt->cur_magic];
    jsobj *proto = rt->proto_error;
    prop *ctor = prop_find(rt->global, atom_c(rt, kind));
    if (ctor && ctor->val.t == JT_OBJ) { prop *pp = prop_find(ctor->val.u.o, rt->a_prototype); if (pp && pp->val.t == JT_OBJ) proto = pp->val.u.o; }
    jsobj *e = obj_new(rt, proto, CL_ERROR);
    if (ARG(0).t != JT_UNDEF) { jstr *m = to_str(rt, ARG(0)); RET_IF_BAD(); prop_put(rt, e, rt->a_message, v_str(m), PF_HIDDEN); }
    return v_obj(e);
}

static jsval n_err_toString(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_OBJ) return throw_error(rt, "TypeError", "Error.prototype.toString called on non-object", NULL, NULL);
    jsval nm = get_prop(rt, thisv, rt->a_name), msg = get_prop(rt, thisv, rt->a_message);
    RET_IF_BAD();
    jstr *ns = nm.t == JT_UNDEF ? atom_c(rt, "Error") : to_str(rt, nm), *ms = msg.t == JT_UNDEF ? rt->a_empty : to_str(rt, msg);
    if (!ms->len) return v_str(ns);
    if (!ns->len) return v_str(ms);
    return v_str(str_cat(rt, str_cat(rt, ns, atom_c(rt, ": ")), ms));
}

/* ---- Date (UTC only) --------------------------------------------------------------------------------------- */

static long long days_from_civil(long long y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, long long *y, int *m, int *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

static long long now_ms(js_rt *rt) { return rt->clock ? rt->clock(rt->clock_ud) : 0; }

static jsval n_Date(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    long long t;
    if (!rt->cur_new) {                                         /* Date() as a function returns a string */
        char buf[40]; int n = num_fmt(num_from_int(now_ms(rt)), buf);
        return v_str(str_new(rt, buf, (unsigned)n));
    }
    if (nargs == 0) t = now_ms(rt);
    else if (nargs == 1) { js_num n = to_num(rt, args[0]); RET_IF_BAD(); t = num_isfin(n) ? n / JS_SCALE : 0; }
    else {
        long long f[7] = { 1970, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < nargs && i < 7; i++) { js_num n = to_num(rt, args[i]); RET_IF_BAD(); f[i] = num_isfin(n) ? n / JS_SCALE : 0; }
        long long y = f[0] + (f[1] >= 0 ? f[1] / 12 : -((11 - f[1]) / 12)), mo = ((f[1] % 12) + 12) % 12;
        t = ((days_from_civil(y, (int)mo + 1, 1) + f[2] - 1) * 24 + f[3]) * 3600000LL + f[4] * 60000LL + f[5] * 1000LL + f[6];
    }
    jsobj *d = obj_new(rt, rt->proto_date, CL_DATE);
    d->thisv = vnum(t);
    return v_obj(d);
}

static jsval n_Date_now(js_rt *rt, jsval thisv, jsval *args, int nargs) { return vnum(now_ms(rt)); }

/* magic: 0 getTime, 1 year, 2 month, 3 date, 4 hours, 5 minutes, 6 seconds, 7 ms, 8 day(of week), 9 toISOString */
static jsval n_date_get(js_rt *rt, jsval thisv, jsval *args, int nargs)
{
    if (thisv.t != JT_OBJ || thisv.u.o->cls != CL_DATE) return throw_error(rt, "TypeError", "this is not a Date object", NULL, NULL);
    long long t = num_to_int64(thisv.u.o->thisv.u.n);
    long long days = t >= 0 ? t / 86400000LL : -((-t + 86399999LL) / 86400000LL);
    long long ms = t - days * 86400000LL, y; int m, d;
    civil_from_days(days, &y, &m, &d);
    switch (rt->cur_magic) {
    case 0: return vnum(t);
    case 1: return vnum(y);
    case 2: return vnum(m - 1);
    case 3: return vnum(d);
    case 4: return vnum(ms / 3600000);
    case 5: return vnum(ms / 60000 % 60);
    case 6: return vnum(ms / 1000 % 60);
    case 7: return vnum(ms % 1000);
    case 8: return vnum(((days % 7) + 11) % 7);
    default: {
        char buf[48];
        int n = 0;
        long long vals[7] = { y, m, d, ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000 };
        int widths[7] = { 4, 2, 2, 2, 2, 2, 3 };
        const char seps[7] = { '-', '-', 'T', ':', ':', '.', 'Z' };
        for (int i = 0; i < 7; i++) {
            long long v = vals[i];
            for (int w = widths[i] - 1; w >= 0; w--) { long long p = 1; for (int k = 0; k < w; k++) p *= 10; buf[n++] = (char)('0' + (v / p) % 10); }
            buf[n++] = seps[i];
        }
        return v_str(str_new(rt, buf, (unsigned)n));
    }
    }
}

/* ---- setup ------------------------------------------------------------------------------------------------- */

static jsobj *make_ctor(js_rt *rt, const char *name, js_native fn, int magic, jsobj *proto)
{
    jsobj *c = native_new(rt, name, fn, 1, magic);
    prop_put(rt, c, rt->a_prototype, v_obj(proto), PF_HIDDEN);
    prop_put(rt, proto, rt->a_constructor, v_obj(c), PF_HIDDEN);
    prop_put(rt, rt->global, atom_c(rt, name), v_obj(c), PF_HIDDEN);
    return c;
}

void builtins_init(js_rt *rt)
{
    jsobj *g = rt->global;
    prop_put(rt, g, atom_c(rt, "globalThis"), v_obj(g), PF_HIDDEN);
    prop_put(rt, g, rt->a_undefined, U, PF_HIDDEN);
    prop_put(rt, g, atom_c(rt, "NaN"), v_num(JS_NAN), PF_HIDDEN);
    prop_put(rt, g, atom_c(rt, "Infinity"), v_num(JS_PINF), PF_HIDDEN);

    jsobj *O = make_ctor(rt, "Object", n_Object, 0, rt->proto_object);
    def_native(rt, O, "keys", n_Object_keys, 1, 0);
    def_native(rt, O, "values", n_Object_keys, 1, 1);
    def_native(rt, O, "entries", n_Object_keys, 1, 2);
    def_native(rt, O, "getOwnPropertyNames", n_Object_keys, 1, 3);
    def_native(rt, O, "assign", n_Object_assign, 2, 0);
    def_native(rt, O, "create", n_Object_create, 2, 0);
    def_native(rt, O, "getPrototypeOf", n_Object_getProto, 1, 0);
    def_native(rt, O, "setPrototypeOf", n_Object_setProto, 2, 0);
    def_native(rt, O, "freeze", n_Object_freeze, 1, 0);
    def_native(rt, O, "defineProperty", n_Object_defineProperty, 3, 0);
    def_native(rt, O, "hasOwn", n_hasOwn, 2, 1);
    def_native(rt, rt->proto_object, "hasOwnProperty", n_hasOwn, 1, 0);
    def_native(rt, rt->proto_object, "toString", n_obj_toString, 0, 0);
    def_native(rt, rt->proto_object, "valueOf", n_obj_valueOf, 0, 0);
    def_native(rt, rt->proto_object, "isPrototypeOf", n_isPrototypeOf, 1, 0);

    make_ctor(rt, "Function", n_Function, 0, rt->proto_function);
    def_native(rt, rt->proto_function, "call", n_fn_call, 1, 0);
    def_native(rt, rt->proto_function, "apply", n_fn_apply, 2, 0);
    def_native(rt, rt->proto_function, "bind", n_fn_bind, 1, 0);
    def_native(rt, rt->proto_function, "toString", n_fn_toString, 0, 0);

    jsobj *A = make_ctor(rt, "Array", n_Array, 0, rt->proto_array);
    def_native(rt, A, "isArray", n_Array_isArray, 1, 0);
    def_native(rt, A, "from", n_Array_from, 1, 0);
    def_native(rt, A, "of", n_Array_of, 0, 0);
    jsobj *ap = rt->proto_array;
    def_native(rt, ap, "push", n_arr_push, 1, 0);
    def_native(rt, ap, "pop", n_arr_pop, 0, 0);
    def_native(rt, ap, "shift", n_arr_shift, 0, 0);
    def_native(rt, ap, "unshift", n_arr_unshift, 1, 0);
    def_native(rt, ap, "slice", n_arr_slice, 2, 0);
    def_native(rt, ap, "splice", n_arr_splice, 2, 0);
    def_native(rt, ap, "concat", n_arr_concat, 1, 0);
    def_native(rt, ap, "join", n_arr_join, 1, 0);
    def_native(rt, ap, "toString", n_arr_join, 0, 0);
    def_native(rt, ap, "reverse", n_arr_reverse, 0, 0);
    def_native(rt, ap, "indexOf", n_arr_indexOf, 1, 0);
    def_native(rt, ap, "lastIndexOf", n_arr_indexOf, 1, 1);
    def_native(rt, ap, "includes", n_arr_indexOf, 1, 2);
    static const char *iters[] = { "forEach", "map", "filter", "some", "every", "find", "findIndex", "findLast", "findLastIndex" };
    for (int i = 0; i < 9; i++) def_native(rt, ap, iters[i], n_arr_iter, 1, i);
    def_native(rt, ap, "reduce", n_arr_reduce, 1, 0);
    def_native(rt, ap, "reduceRight", n_arr_reduce, 1, 1);
    def_native(rt, ap, "sort", n_arr_sort, 1, 0);
    def_native(rt, ap, "fill", n_arr_fill, 1, 0);
    def_native(rt, ap, "flat", n_arr_flat, 0, 0);
    def_native(rt, ap, "at", n_arr_at, 1, 0);

    jsobj *S = make_ctor(rt, "String", n_String, 0, rt->proto_string);
    def_native(rt, S, "fromCharCode", n_String_fromCharCode, 1, 0);
    jsobj *sp = rt->proto_string;
    def_native(rt, sp, "charAt", n_str_charAt, 1, 0);
    def_native(rt, sp, "charCodeAt", n_str_charAt, 1, 1);
    def_native(rt, sp, "codePointAt", n_str_charAt, 1, 1);
    def_native(rt, sp, "at", n_str_charAt, 1, 2);
    def_native(rt, sp, "indexOf", n_str_indexOf, 1, 0);
    def_native(rt, sp, "lastIndexOf", n_str_indexOf, 1, 1);
    def_native(rt, sp, "includes", n_str_indexOf, 1, 2);
    def_native(rt, sp, "startsWith", n_str_indexOf, 1, 3);
    def_native(rt, sp, "endsWith", n_str_indexOf, 1, 4);
    def_native(rt, sp, "slice", n_str_slice, 2, 0);
    def_native(rt, sp, "substring", n_str_slice, 2, 1);
    def_native(rt, sp, "substr", n_str_slice, 2, 2);
    def_native(rt, sp, "toUpperCase", n_str_case, 0, 0);
    def_native(rt, sp, "toLowerCase", n_str_case, 0, 1);
    def_native(rt, sp, "trim", n_str_trim, 0, 0);
    def_native(rt, sp, "trimStart", n_str_trim, 0, 1);
    def_native(rt, sp, "trimEnd", n_str_trim, 0, 2);
    def_native(rt, sp, "split", n_str_split, 2, 0);
    def_native(rt, sp, "replace", n_str_replace, 2, 0);
    def_native(rt, sp, "replaceAll", n_str_replace, 2, 1);
    def_native(rt, sp, "repeat", n_str_repeat, 1, 0);
    def_native(rt, sp, "padStart", n_str_pad, 2, 0);
    def_native(rt, sp, "padEnd", n_str_pad, 2, 1);
    def_native(rt, sp, "concat", n_str_concat, 1, 0);
    def_native(rt, sp, "localeCompare", n_str_localeCompare, 1, 0);
    def_native(rt, sp, "toString", n_str_toString, 0, 0);
    def_native(rt, sp, "valueOf", n_str_toString, 0, 0);

    jsobj *N = make_ctor(rt, "Number", n_Number, 0, rt->proto_number);
    def_native(rt, N, "isInteger", n_Number_isInteger, 1, 0);
    def_native(rt, N, "isSafeInteger", n_Number_isInteger, 1, 0);
    def_native(rt, N, "isNaN", n_isNaN, 1, 1);
    def_native(rt, N, "isFinite", n_isFinite, 1, 1);
    def_native(rt, N, "parseFloat", n_parseFloat, 1, 0);
    def_native(rt, N, "parseInt", n_parseInt, 2, 0);
    set_hidden(rt, N, "MAX_SAFE_INTEGER", v_num(JS_LIMIT / JS_SCALE * JS_SCALE));   /* this engine's numbers stop at 4e13 */
    set_hidden(rt, N, "MIN_SAFE_INTEGER", v_num(-(JS_LIMIT / JS_SCALE * JS_SCALE)));
    set_hidden(rt, N, "EPSILON", v_num(1));
    set_hidden(rt, N, "POSITIVE_INFINITY", v_num(JS_PINF));
    set_hidden(rt, N, "NEGATIVE_INFINITY", v_num(JS_NINF));
    set_hidden(rt, N, "NaN", v_num(JS_NAN));
    def_native(rt, rt->proto_number, "toString", n_num_toString, 1, 0);
    def_native(rt, rt->proto_number, "toLocaleString", n_num_toString, 0, 0);
    def_native(rt, rt->proto_number, "toFixed", n_num_toFixed, 1, 0);
    def_native(rt, rt->proto_number, "valueOf", n_num_valueOf, 0, 0);
    make_ctor(rt, "Boolean", n_Boolean, 0, rt->proto_boolean);
    def_native(rt, rt->proto_boolean, "toString", n_bool_toString, 0, 0);
    def_native(rt, rt->proto_boolean, "valueOf", n_num_valueOf, 0, 0);

    def_native(rt, g, "parseInt", n_parseInt, 2, 0);
    def_native(rt, g, "parseFloat", n_parseFloat, 1, 0);
    def_native(rt, g, "isNaN", n_isNaN, 1, 0);
    def_native(rt, g, "isFinite", n_isFinite, 1, 0);

    jsobj *M = obj_new(rt, rt->proto_object, CL_OBJECT);
    prop_put(rt, g, atom_c(rt, "Math"), v_obj(M), PF_HIDDEN);
    static const char *mfn[] = { "floor", "ceil", "round", "trunc", "abs", "sqrt", "sign", "pow", "min", "max", "random", "hypot" };
    for (int i = 0; i < 12; i++) def_native(rt, M, mfn[i], n_Math, 1, i);
    set_hidden(rt, M, "PI", v_num(31416));
    set_hidden(rt, M, "E", v_num(27183));

    jsobj *J = obj_new(rt, rt->proto_object, CL_OBJECT);
    prop_put(rt, g, atom_c(rt, "JSON"), v_obj(J), PF_HIDDEN);
    def_native(rt, J, "stringify", n_JSON_stringify, 3, 0);
    def_native(rt, J, "parse", n_JSON_parse, 2, 0);

    static const char *ekinds[] = { "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError" };
    for (int i = 0; i < 7; i++) {
        jsobj *proto = i == 0 ? rt->proto_error : obj_new(rt, rt->proto_error, CL_OBJECT);
        make_ctor(rt, ekinds[i], n_Error, i, proto);
        set_hidden(rt, proto, "name", v_str(atom_c(rt, ekinds[i])));
        set_hidden(rt, proto, "message", v_str(rt->a_empty));
    }
    def_native(rt, rt->proto_error, "toString", n_err_toString, 0, 0);

    jsobj *D = make_ctor(rt, "Date", n_Date, 0, rt->proto_date);
    def_native(rt, D, "now", n_Date_now, 0, 0);
    static const char *dfn[] = { "getTime", "getUTCFullYear", "getUTCMonth", "getUTCDate", "getUTCHours", "getUTCMinutes", "getUTCSeconds", "getUTCMilliseconds", "getUTCDay", "toISOString" };
    for (int i = 0; i < 10; i++) def_native(rt, rt->proto_date, dfn[i], n_date_get, 0, i);
    def_native(rt, rt->proto_date, "valueOf", n_date_get, 0, 0);
    def_native(rt, rt->proto_date, "toString", n_date_get, 0, 9);
    def_native(rt, rt->proto_date, "toJSON", n_date_get, 0, 9);
    static const char *dfl[] = { "getFullYear", "getMonth", "getDate", "getHours", "getMinutes", "getSeconds", "getMilliseconds", "getDay" };
    for (int i = 0; i < 8; i++) def_native(rt, rt->proto_date, dfl[i], n_date_get, 0, i + 1);

    jsobj *C = obj_new(rt, rt->proto_object, CL_OBJECT);
    prop_put(rt, g, atom_c(rt, "console"), v_obj(C), PF_HIDDEN);
    static const char *cfn[] = { "log", "info", "warn", "error", "debug" };
    for (int i = 0; i < 5; i++) def_native(rt, C, cfn[i], n_console_log, 0, 0);
    def_native(rt, g, "print", n_console_log, 0, 0);
}

/* ---- public API -------------------------------------------------------------------------------------------- */

js_rt *js_new(js_out_fn out, void *ud)
{
    js_rt *rt = (js_rt *)JS_MALLOC(sizeof(js_rt));
    if (!rt) return NULL;
    memset(rt, 0, sizeof *rt);
    rt->out = out; rt->out_ud = ud;
    rt->max_bytes = 32ul * 1024 * 1024;
    rt->max_steps = 100000000ul;
    rt->max_depth = 400;
    rt->gc_mark_bytes = 1024 * 1024;
    rt->rng = 0x9E3779B97F4A7C15ULL;
    /* the dummy cells exist before anything else so allocation failures always have something to return */
    rt->dummy_str = (jstr *)JS_MALLOC(sizeof(jstr) + 1);
    rt->dummy_obj = (jsobj *)JS_MALLOC(sizeof(jsobj));
    if (!rt->dummy_str || !rt->dummy_obj) { JS_FREE(rt); return NULL; }
    memset(rt->dummy_str, 0, sizeof(jstr) + 1);
    memset(rt->dummy_obj, 0, sizeof(jsobj));
    rt->dummy_str->h.kind = GK_STR; rt->dummy_str->atom = 1;
    rt->dummy_obj->h.kind = GK_OBJ; rt->dummy_obj->cls = CL_OBJECT;
    rt->dummy_str->h.next = (gcc *)rt->dummy_obj;
    rt->heap = &rt->dummy_str->h;
    rt->exc = v_undef(); rt->ret = v_undef();
#define A(f, s) rt->f = atom_c(rt, s)
    A(a_empty, ""); A(a_length, "length"); A(a_prototype, "prototype"); A(a_constructor, "constructor"); A(a_name, "name");
    A(a_message, "message"); A(a_tostring, "toString"); A(a_valueof, "valueOf"); A(a_proto, "__proto__");
    A(a_arguments, "arguments"); A(a_this, "this"); A(a_index, "index"); A(a_undefined, "undefined"); A(a_null, "null");
    A(a_true, "true"); A(a_false, "false"); A(a_object, "object"); A(a_function, "function"); A(a_number, "number");
    A(a_string, "string"); A(a_boolean, "boolean"); A(a_nan, "NaN"); A(a_infinity, "Infinity"); A(a_get, "get");
    A(a_set, "set"); A(a_callee, "callee");
#undef A
    rt->proto_object = obj_new(rt, NULL, CL_OBJECT);
    rt->proto_function = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->proto_array = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->proto_string = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->proto_number = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->proto_boolean = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->proto_error = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->proto_date = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->global = obj_new(rt, rt->proto_object, CL_OBJECT);
    rt->global->has_this = 1;
    rt->global->thisv = v_obj(rt->global);
    builtins_init(rt);
    if (rt->abort_msg) { js_free(rt); return NULL; }
    rt->gc_mark_bytes = rt->bytes + 1024 * 1024;
    return rt;
}

void js_free(js_rt *rt)
{
    if (!rt) return;
    gcc *c = rt->heap;
    while (c) {
        gcc *n = c->next;
        if (c != &rt->dummy_str->h && c != &rt->dummy_obj->h) {
            if (c->kind == GK_OBJ) {
                jsobj *o = (jsobj *)c;
                if (o->props) JS_FREE(o->props);
                if (o->items) JS_FREE(o->items);
                if (o->bound_args) JS_FREE(o->bound_args);
            }
            JS_FREE(c);
        }
        c = n;
    }
    JS_FREE(rt->dummy_str);
    JS_FREE(rt->dummy_obj);
    arena_blk *a = rt->arena;
    while (a) { arena_blk *n = a->next; JS_FREE(a); a = n; }
    if (rt->atoms) JS_FREE(rt->atoms);
    if (rt->pins) JS_FREE(rt->pins);
    JS_FREE(rt);
}

void js_set_limits(js_rt *rt, unsigned long max_bytes, unsigned long max_steps, int max_depth)
{
    if (max_bytes) rt->max_bytes = max_bytes;
    if (max_steps) rt->max_steps = max_steps;
    if (max_depth) rt->max_depth = max_depth;
}

void js_set_clock(js_rt *rt, long long (*fn)(void *ud), void *ud) { rt->clock = fn; rt->clock_ud = ud; }

unsigned long js_mem_used(js_rt *rt) { return rt->bytes; }

void js_gc(js_rt *rt) { gc_run(rt); }
void js_set_gc_stress(js_rt *rt, unsigned n) { rt->gc_stress = n; }

int js_eval(js_rt *rt, const char *src, int len, char *err, int errmax)
{
    rt->abort_msg = NULL;
    node *prog = js_parse(rt, src, len, err, errmax);
    if (!prog) {
        if (rt->abort_msg) { const char *m = rt->abort_msg; rt->abort_msg = NULL; if (err && errmax > 0) { int n = 0; while (m[n] && n < errmax - 1) { err[n] = m[n]; n++; } err[n] = 0; } }
        else if (err && errmax > 0 && !err[0]) { const char *m = "SyntaxError: parse failed"; int n = 0; while (m[n] && n < errmax - 1) { err[n] = m[n]; n++; } err[n] = 0; }
        return 1;
    }
    int r = js_run(rt, prog, err, errmax);
    if (rt->bytes > rt->gc_mark_bytes) gc_run(rt);
    return r;
}

/* Runs source and returns the value of its last expression statement (used for onclick="..." attributes). */
int js_eval_value(js_rt *rt, const char *src, int len, jsval *out, char *err, int errmax)
{
    rt->abort_msg = NULL;
    rt->completion = v_undef();
    node *prog = js_parse(rt, src, len, err, errmax);
    if (!prog) {
        if (err && errmax > 0 && !err[0]) { const char *m = "SyntaxError: parse failed"; int n = 0; while (m[n] && n < errmax - 1) { err[n] = m[n]; n++; } err[n] = 0; }
        rt->abort_msg = NULL;
        return 1;
    }
    int r = js_run(rt, prog, err, errmax);
    if (out) *out = r ? v_undef() : rt->completion;
    return r;
}
