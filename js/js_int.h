/* NimbusOS JavaScript engine - internal types shared by the engine's files. */
#ifndef JS_INT_H
#define JS_INT_H
#include "js/js_plat.h"
#include "js/js.h"

/* ---- numbers: fixed point, 4 decimals, no floating point anywhere --------------------------------------------
 * A js_num is the value times JS_SCALE. The largest magnitude is JS_LIMIT/JS_SCALE (4e13, so millisecond
 * timestamps fit); anything bigger becomes +-Infinity. NaN and the infinities are sentinel values. Differences
 * from real JavaScript: 4 decimal places (1/3 is 0.3333), no exponent notation, no -0. */
typedef long long js_num;
#define JS_SCALE 10000LL
#define JS_LIMIT 400000000000000000LL
#define JS_NAN   0x7FFFFFFFFFFFFFFFLL
#define JS_PINF  0x7FFFFFFFFFFFFFFELL
#define JS_NINF  (-0x7FFFFFFFFFFFFFFELL)
static inline int num_isnan(js_num n) { return n == JS_NAN; }
static inline int num_isinf(js_num n) { return n == JS_PINF || n == JS_NINF; }
static inline int num_isfin(js_num n) { return n != JS_NAN && n != JS_PINF && n != JS_NINF; }

js_num num_from_int(long long i);
long long num_to_int64(js_num n);              /* truncates toward zero; 0 for NaN/Infinity */
int    num_to_int32(js_num n);                 /* ToInt32 */
js_num num_add(js_num a, js_num b);
js_num num_sub(js_num a, js_num b);
js_num num_mul(js_num a, js_num b);
js_num num_div(js_num a, js_num b);
js_num num_mod(js_num a, js_num b);
js_num num_neg(js_num a);
int    num_cmp(js_num a, js_num b);            /* -1, 0, 1, or 2 when unordered (NaN) */
js_num num_floor(js_num a);
js_num num_ceil(js_num a);
js_num num_round(js_num a);
js_num num_trunc(js_num a);
js_num num_pow(js_num a, js_num b);
js_num num_sqrt(js_num a);
int    num_fmt(js_num n, char *buf);           /* writes up to 32 chars, returns the length */
int    num_fmt_radix(js_num n, int radix, char *buf);
int    num_fmt_fixed(js_num n, int digits, char *buf);
js_num num_parse(const char *s, int len);      /* Number("..."): whole string must be a number, else NaN */
js_num num_parse_prefix(const char *s, int len, int *used);   /* parseFloat: longest numeric prefix */

/* ---- values and heap cells -------------------------------------------------------------------------------- */
typedef struct jstr jstr;
typedef struct jsobj jsobj;
typedef struct node node;

enum { JT_UNDEF, JT_NULL, JT_BOOL, JT_NUM, JT_STR, JT_OBJ };
typedef struct { int t; union { int b; js_num n; jstr *s; jsobj *o; } u; } jsval;

static inline jsval v_undef(void) { jsval v; v.t = JT_UNDEF; v.u.n = 0; return v; }
static inline jsval v_null(void)  { jsval v; v.t = JT_NULL;  v.u.n = 0; return v; }
static inline jsval v_bool(int b) { jsval v; v.t = JT_BOOL;  v.u.n = 0; v.u.b = b != 0; return v; }
static inline jsval v_num(js_num n) { jsval v; v.t = JT_NUM; v.u.n = n; return v; }
static inline jsval v_str(jstr *s) { jsval v; v.t = JT_STR; v.u.n = 0; v.u.s = s; return v; }
static inline jsval v_obj(jsobj *o) { jsval v; v.t = JT_OBJ; v.u.n = 0; v.u.o = o; return v; }

typedef struct gcc { struct gcc *next; unsigned char kind, mark; } gcc;
enum { GK_STR, GK_OBJ };

struct jstr {
    gcc h;
    unsigned len, hash;
    unsigned char atom;        /* interned: equal atoms are the same pointer */
    char s[1];                 /* len bytes + NUL */
};

typedef jsval (*js_native)(js_rt *rt, jsval thisv, jsval *args, int nargs);

typedef struct { jstr *key; jsval val; unsigned char flags; } prop;
#define PF_HIDDEN 1            /* not enumerable */
#define PF_CONST  2            /* const binding: assignment throws */

enum { CL_OBJECT, CL_ARRAY, CL_FUNCTION, CL_SCOPE, CL_ERROR, CL_DATE };

struct jsobj {
    gcc h;
    unsigned char cls;
    unsigned char has_this;                 /* scope: this function scope binds `this` */
    unsigned char is_arrow;
    unsigned char extensible;
    jsobj *proto;
    prop *props; int np, pcap;
    jsval *items; unsigned alen, acap;      /* arrays */
    node *fnode; jsobj *env;                /* closures: function node + defining scope */
    js_native native; int magic;            /* native functions */
    jsval bound_this; jsobj *bound_fn; jsval *bound_args; int nbound;
    jsobj *outer;                           /* scopes: enclosing scope */
    jsval thisv;                            /* scopes with has_this; Date: time value */
};

/* ---- AST --------------------------------------------------------------------------------------------------- */
enum {
    N_NUM, N_STR, N_TMPL, N_IDENT, N_THIS, N_NULL, N_TRUE, N_FALSE, N_ARRAY, N_OBJECT, N_PROP, N_FUNC,
    N_MEMBER, N_INDEX, N_CALL, N_NEW, N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_SEQ, N_SPREAD,
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_FOR, N_FORIN, N_FOROF, N_WHILE, N_DOWHILE, N_RETURN, N_BREAK,
    N_CONTINUE, N_THROW, N_TRY, N_SWITCH, N_CASE, N_EMPTY, N_FUNCDECL
};

/* operator codes (node.op) */
enum {
    O_ADD = 1, O_SUB, O_MUL, O_DIV, O_MOD, O_POW, O_SHL, O_SHR, O_USHR, O_BAND, O_BOR, O_BXOR,
    O_EQ, O_NE, O_SEQ, O_SNE, O_LT, O_GT, O_LE, O_GE, O_IN, O_INSTANCEOF,
    O_AND, O_OR, O_NOT, O_BNOT, O_TYPEOF, O_VOID, O_DELETE, O_PLUS, O_MINUS, O_ASSIGN, O_INC, O_DEC, O_NULLISH
};

struct node {
    unsigned char type, op, flag;           /* flag: N_FUNC arrow / N_VAR kind / N_UPDATE prefix / N_BLOCK lexical */
    int line;
    node *a, *b, *c, *d;
    node *next;
    jstr *str;                              /* identifier / property name / string value */
    js_num num;
    /* N_FUNC */
    node *params, *body, *vars, *funcs;     /* params: N_IDENT list; vars: hoisted var names; funcs: N_FUNCDECL list */
    int nparams; unsigned char uses_args, expr_body;
};
enum { VK_VAR, VK_LET, VK_CONST };

/* ---- runtime ------------------------------------------------------------------------------------------------ */
typedef struct arena_blk { struct arena_blk *next; unsigned used, size; } arena_blk;

struct js_rt {
    js_out_fn out; void *out_ud;
    gcc *heap; unsigned long bytes, max_bytes, gc_mark_bytes;
    unsigned long steps, max_steps; int depth, max_depth;
    jstr **atoms; unsigned atom_cap, atom_count;
    arena_blk *arena;
    jsobj *global;
    jsobj *proto_object, *proto_function, *proto_array, *proto_string, *proto_number, *proto_boolean, *proto_error;
    jsobj *proto_date;
    int throwing; jsval exc;                /* a JS exception is propagating */
    const char *abort_msg;                  /* uncatchable stop: step limit, out of memory, ... */
    jsval ret;                              /* value of the last `return` */
    jsval *pins; int npins, pincap;         /* extra GC roots */
    unsigned long long rng;
    jstr *dummy_str; jsobj *dummy_obj;
    const char *src; int srclen;
    /* frequently used atoms */
    jstr *a_length, *a_prototype, *a_constructor, *a_name, *a_message, *a_tostring, *a_valueof, *a_proto,
         *a_empty, *a_arguments, *a_this, *a_index, *a_undefined, *a_null, *a_true, *a_false, *a_object,
         *a_function, *a_number, *a_string, *a_boolean, *a_nan, *a_infinity, *a_get, *a_set, *a_callee;
    long long (*clock)(void *ud); void *clock_ud;   /* milliseconds since 1970, for Date */
    int cur_magic, cur_new;                 /* set before a native function runs */
    int scratch_depth;
};

/* strings, atoms, cells (obj.c) */
jstr  *str_new(js_rt *rt, const char *s, unsigned len);
jstr  *str_cat(js_rt *rt, jstr *a, jstr *b);
jstr  *atom_new(js_rt *rt, const char *s, unsigned len);
jstr  *atom_c(js_rt *rt, const char *s);
jstr  *to_atom(js_rt *rt, jstr *s);
int    str_eq(jstr *a, jstr *b);
int    atom_index(jstr *a, unsigned *idx);         /* "12" -> 12 */
jsobj *obj_new(js_rt *rt, jsobj *proto, int cls);
jsobj *arr_new(js_rt *rt, unsigned n);
void   arr_push(js_rt *rt, jsobj *a, jsval v);
void   arr_set_len(js_rt *rt, jsobj *a, unsigned n);
prop  *prop_find(jsobj *o, jstr *key);
void   prop_put(js_rt *rt, jsobj *o, jstr *key, jsval v, int flags);   /* define or overwrite an own property */
int    prop_del(jsobj *o, jstr *key);
void   js_pin(js_rt *rt, jsval v);
void   gc_run(js_rt *rt);
void  *arena_alloc(js_rt *rt, unsigned n);

/* string builder */
typedef struct { char *p; unsigned len, cap; int oom; } sbuf;
void sb_init(sbuf *b);
void sb_add(sbuf *b, const char *s, unsigned n);
void sb_addc(sbuf *b, char c);
void sb_adds(sbuf *b, const char *cstr);
void sb_free(sbuf *b);
jstr *sb_str(js_rt *rt, sbuf *b);

/* conversions and operations (eval.c) */
jstr  *to_str(js_rt *rt, jsval v);
js_num to_num(js_rt *rt, jsval v);
int    to_bool(jsval v);
jsval  to_prim(js_rt *rt, jsval v, int prefer_string);
jstr  *to_key_atom(js_rt *rt, jsval v);
jsval  get_member(js_rt *rt, jsval base, jsval key);
jsval  get_prop(js_rt *rt, jsval base, jstr *key);       /* key is an atom */
void   set_member(js_rt *rt, jsval base, jsval key, jsval v);
void   set_prop(js_rt *rt, jsval base, jstr *key, jsval v);
jsval  call_fn(js_rt *rt, jsval fn, jsval thisv, jsval *args, int nargs);
jsval  construct(js_rt *rt, jsval fn, jsval *args, int nargs);
int    strict_eq(jsval a, jsval b);
int    loose_eq(js_rt *rt, jsval a, jsval b);
int    is_callable(jsval v);
jsval  throw_error(js_rt *rt, const char *kind, const char *m1, const char *m2, const char *m3);
const char *type_of(jsval v);
int    js_run(js_rt *rt, node *prog, char *err, int errmax);
jsobj *own_keys(js_rt *rt, jsobj *o, int all);              /* array of key strings (enumerable only unless all) */
jsobj *make_closure(js_rt *rt, node *f, jsobj *scope);
void   set_hidden(js_rt *rt, jsobj *o, const char *name, jsval v);

/* parser (parse.c) */
node  *js_parse(js_rt *rt, const char *src, int len, char *err, int errmax);

/* builtins (builtins.c) */
void   builtins_init(js_rt *rt);
jsobj *native_new(js_rt *rt, const char *name, js_native fn, int nargs, int magic);
void   def_native(js_rt *rt, jsobj *o, const char *name, js_native fn, int nargs, int magic);
void   inspect(js_rt *rt, jsval v, sbuf *b, int depth, int top);
jstr  *json_quote(js_rt *rt, jstr *s);

#define NOVAL(rt) ((rt)->throwing || (rt)->abort_msg)

#endif
