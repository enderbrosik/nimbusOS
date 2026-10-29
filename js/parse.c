/* NimbusOS JavaScript engine - lexer and parser (source text -> AST). */
#include "js/js_int.h"

enum {
    T_EOF, T_NUM, T_STR, T_TMPL, T_IDENT,
    /* keywords */
    T_VAR, T_LET, T_CONST, T_FUNCTION, T_RETURN, T_IF, T_ELSE, T_FOR, T_WHILE, T_DO, T_BREAK, T_CONTINUE, T_NEW,
    T_THIS, T_NULL, T_TRUE, T_FALSE, T_TYPEOF, T_VOID, T_DELETE, T_IN, T_INSTANCEOF, T_THROW, T_TRY, T_CATCH,
    T_FINALLY, T_SWITCH, T_CASE, T_DEFAULT,
    /* punctuation */
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_SEMI, T_COMMA, T_DOT, T_ELLIPSIS, T_QUESTION,
    T_COLON, T_ARROW, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR, T_INC, T_DEC, T_AMP, T_PIPE,
    T_CARET, T_TILDE, T_BANG, T_ANDAND, T_OROR, T_QQ, T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE, T_SEQ, T_SNE, T_SHL,
    T_SHR, T_USHR, T_ASSIGN, T_ASSIGN_OP
};

static const struct { const char *w; int t; } keywords[] = {
    {"var", T_VAR}, {"let", T_LET}, {"const", T_CONST}, {"function", T_FUNCTION}, {"return", T_RETURN},
    {"if", T_IF}, {"else", T_ELSE}, {"for", T_FOR}, {"while", T_WHILE}, {"do", T_DO}, {"break", T_BREAK},
    {"continue", T_CONTINUE}, {"new", T_NEW}, {"this", T_THIS}, {"null", T_NULL}, {"true", T_TRUE},
    {"false", T_FALSE}, {"typeof", T_TYPEOF}, {"void", T_VOID}, {"delete", T_DELETE}, {"in", T_IN},
    {"instanceof", T_INSTANCEOF}, {"throw", T_THROW}, {"try", T_TRY}, {"catch", T_CATCH}, {"finally", T_FINALLY},
    {"switch", T_SWITCH}, {"case", T_CASE}, {"default", T_DEFAULT}, {0, 0}
};

typedef struct {
    int type, op, nl_before, line;
    jstr *str;                 /* identifiers, keywords, strings */
    js_num num;
    int start, end;            /* T_TMPL: raw text range */
} token;

typedef struct {
    js_rt *rt;
    const char *s; int len, pos, line;
    token tok;
    int prev_end_line;
    node *fn;                  /* function being parsed */
    int failed, no_in, depth, block_lex;
    char *err; int errmax;
} parser;

#define MAX_PARSE_DEPTH 160

static void perr(parser *p, const char *msg)
{
    if (p->failed) return;
    p->failed = 1;
    if (p->err && p->errmax > 0) {
        int n = 0;
        const char *pre = "SyntaxError: ";
        while (*pre && n < p->errmax - 1) p->err[n++] = *pre++;
        while (*msg && n < p->errmax - 1) p->err[n++] = *msg++;
        const char *ln = " (line ";
        while (*ln && n < p->errmax - 1) p->err[n++] = *ln++;
        char t[12]; int tn = 0, v = p->tok.line;
        do { t[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn && n < p->errmax - 1) p->err[n++] = t[--tn];
        if (n < p->errmax - 1) p->err[n++] = ')';
        p->err[n] = 0;
    }
}

/* ---- lexer ------------------------------------------------------------------------------------------------- */

static int is_id_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80; }
static int is_id_char(int c) { return is_id_start(c) || (c >= '0' && c <= '9'); }
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_put(sbuf *b, unsigned cp)
{
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); n = 4; }
    sb_add(b, t, (unsigned)n);
}

/* Reads one escape sequence after the backslash at s[*i]; appends the result to b. */
static int read_escape(parser *p, const char *s, int end, int *i, sbuf *b, int in_tmpl)
{
    char c = s[(*i)++];
    switch (c) {
    case 'n': sb_addc(b, '\n'); break;
    case 'r': sb_addc(b, '\r'); break;
    case 't': sb_addc(b, '\t'); break;
    case 'b': sb_addc(b, '\b'); break;
    case 'f': sb_addc(b, '\f'); break;
    case 'v': sb_addc(b, '\v'); break;
    case '0': sb_addc(b, 0); break;
    case '\r': if (*i < end && s[*i] == '\n') (*i)++; p->line++; break;
    case '\n': p->line++; break;
    case 'x': {
        int h1 = *i < end ? hexval(s[*i]) : -1, h2 = *i + 1 < end ? hexval(s[*i + 1]) : -1;
        if (h1 < 0 || h2 < 0) { perr(p, "Invalid hexadecimal escape sequence"); return 0; }
        utf8_put(b, (unsigned)(h1 * 16 + h2));
        *i += 2;
        break;
    }
    case 'u': {
        unsigned cp = 0;
        if (*i < end && s[*i] == '{') {
            (*i)++;
            while (*i < end && s[*i] != '}') { int h = hexval(s[*i]); if (h < 0) { perr(p, "Invalid Unicode escape sequence"); return 0; } cp = cp * 16 + (unsigned)h; (*i)++; }
            (*i)++;
        } else {
            for (int k = 0; k < 4; k++) {
                int h = *i < end ? hexval(s[*i]) : -1;
                if (h < 0) { perr(p, "Invalid Unicode escape sequence"); return 0; }
                cp = cp * 16 + (unsigned)h; (*i)++;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && *i + 5 < end && s[*i] == '\\' && s[*i + 1] == 'u') {   /* surrogate pair */
                unsigned lo = 0; int ok = 1;
                for (int k = 0; k < 4; k++) { int h = hexval(s[*i + 2 + k]); if (h < 0) { ok = 0; break; } lo = lo * 16 + (unsigned)h; }
                if (ok && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); *i += 6; }
            }
        }
        utf8_put(b, cp);
        break;
    }
    default:
        (void)in_tmpl;
        sb_addc(b, c);
    }
    return 1;
}

static void lex(parser *p)
{
    const char *s = p->s;
    int len = p->len, i = p->pos;
    token *t = &p->tok;
    p->prev_end_line = p->line;
    t->nl_before = 0;
    for (;;) {                                                   /* whitespace and comments */
        if (i >= len) break;
        char c = s[i];
        if (c == '\n') { t->nl_before = 1; p->line++; i++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') i++;
        else if ((unsigned char)c == 0xC2 && i + 1 < len && (unsigned char)s[i + 1] == 0xA0) i += 2;   /* nbsp */
        else if (c == '/' && i + 1 < len && s[i + 1] == '/') { while (i < len && s[i] != '\n') i++; }
        else if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') { t->nl_before = 1; p->line++; } i++; }
            i += 2;
        }
        else break;
    }
    t->line = p->line;
    t->str = NULL;
    t->op = 0;
    t->num = 0;
    if (i >= len) { t->type = T_EOF; p->pos = len; return; }
    char c = s[i];

    if (is_id_start((unsigned char)c)) {
        int st = i;
        while (i < len && is_id_char((unsigned char)s[i])) i++;
        t->str = atom_new(p->rt, s + st, (unsigned)(i - st));
        t->type = T_IDENT;
        for (int k = 0; keywords[k].w; k++)
            if (t->str->len == (unsigned)strlen(keywords[k].w) && memcmp(t->str->s, keywords[k].w, t->str->len) == 0) { t->type = keywords[k].t; break; }
        p->pos = i;
        return;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')) {
        t->type = T_NUM;
        if (c == '0' && i + 1 < len && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B' || s[i + 1] == 'o' || s[i + 1] == 'O')) {
            int base = (s[i + 1] == 'x' || s[i + 1] == 'X') ? 16 : (s[i + 1] == 'b' || s[i + 1] == 'B') ? 2 : 8;
            i += 2;
            js_num v = 0; int nd = 0;
            while (i < len && ((hexval(s[i]) >= 0 && hexval(s[i]) < base) || s[i] == '_')) { if (s[i] != '_') { v = v * base + hexval(s[i]); nd++; if (v > JS_LIMIT / JS_SCALE) v = JS_LIMIT / JS_SCALE + 1; } i++; }
            if (!nd) perr(p, "Invalid number");
            t->num = num_from_int(v);
        } else {
            char tmp[80]; int n = 0, st = i;
            while (i < len && (s[i] == '_' || (s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
                   ((s[i] == 'e' || s[i] == 'E') && i + 1 < len && (s[i + 1] == '+' || s[i + 1] == '-' || (s[i + 1] >= '0' && s[i + 1] <= '9'))) ||
                   ((s[i] == '+' || s[i] == '-') && i > st && (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
                if (s[i] == '.' ) {
                    /* a second '.' (or a method call like 1..toString) ends the number */
                    int dots = 0; for (int k = st; k < i; k++) if (s[k] == '.') dots++;
                    if (dots) break;
                }
                if (s[i] != '_' && n < 79) tmp[n++] = s[i];
                i++;
            }
            int used;
            t->num = num_parse_prefix(tmp, n, &used);
            if (used != n) perr(p, "Invalid number");
        }
        if (i < len && is_id_start((unsigned char)s[i])) perr(p, "Identifier directly after number");
        p->pos = i;
        return;
    }
    if (c == '"' || c == '\'') {
        sbuf b; sb_init(&b);
        int q = c; i++;
        while (i < len && s[i] != q) {
            if (s[i] == '\n') { perr(p, "Invalid or unexpected token"); break; }
            if (s[i] == '\\') { i++; if (i >= len || !read_escape(p, s, len, &i, &b, 0)) break; }
            else sb_addc(&b, s[i++]);
        }
        if (i >= len) perr(p, "Invalid or unexpected token");
        i++;
        t->type = T_STR;
        t->str = atom_new(p->rt, b.p ? b.p : "", b.len);
        sb_free(&b);
        p->pos = i;
        return;
    }
    if (c == '`') {
        int st = ++i, depth = 0;
        while (i < len) {
            if (s[i] == '\\') { i += 2; continue; }
            if (s[i] == '\n') p->line++;
            if (s[i] == '`' && depth == 0) break;
            if (s[i] == '$' && i + 1 < len && s[i + 1] == '{') { depth++; i += 2; continue; }
            if (s[i] == '}' && depth > 0) depth--;
            if (s[i] == '`' && depth > 0) {                      /* nested template inside ${ } */
                i++;
                int d2 = 0;
                while (i < len && !(s[i] == '`' && d2 == 0)) {
                    if (s[i] == '\\') { i += 2; continue; }
                    if (s[i] == '$' && i + 1 < len && s[i + 1] == '{') { d2++; i += 2; continue; }
                    if (s[i] == '}' && d2 > 0) d2--;
                    i++;
                }
            }
            i++;
        }
        if (i >= len) { perr(p, "Unterminated template literal"); }
        t->type = T_TMPL;
        t->start = st; t->end = i;
        p->pos = i + 1;
        return;
    }
    /* punctuation */
    p->pos = i + 1;
    char c1 = i + 1 < len ? s[i + 1] : 0, c2 = i + 2 < len ? s[i + 2] : 0, c3 = i + 3 < len ? s[i + 3] : 0;
#define TOK(ty) do { t->type = (ty); return; } while (0)
#define TOK2(ty) do { t->type = (ty); p->pos = i + 2; return; } while (0)
#define TOK3(ty) do { t->type = (ty); p->pos = i + 3; return; } while (0)
#define AOP(o, n) do { t->type = T_ASSIGN_OP; t->op = (o); p->pos = i + (n); return; } while (0)
    switch (c) {
    case '{': TOK(T_LBRACE);   case '}': TOK(T_RBRACE);
    case '(': TOK(T_LPAREN);   case ')': TOK(T_RPAREN);
    case '[': TOK(T_LBRACK);   case ']': TOK(T_RBRACK);
    case ';': TOK(T_SEMI);     case ',': TOK(T_COMMA);
    case '~': TOK(T_TILDE);    case ':': TOK(T_COLON);
    case '.': if (c1 == '.' && c2 == '.') TOK3(T_ELLIPSIS); TOK(T_DOT);
    case '?': if (c1 == '?') { if (c2 == '=') { perr(p, "Unsupported operator ?" "?="); TOK(T_EOF); } TOK2(T_QQ); } TOK(T_QUESTION);
    case '+': if (c1 == '+') TOK2(T_INC); if (c1 == '=') AOP(O_ADD, 2); TOK(T_PLUS);
    case '-': if (c1 == '-') TOK2(T_DEC); if (c1 == '=') AOP(O_SUB, 2); TOK(T_MINUS);
    case '*': if (c1 == '*') { if (c2 == '=') AOP(O_POW, 3); TOK2(T_STARSTAR); } if (c1 == '=') AOP(O_MUL, 2); TOK(T_STAR);
    case '/': if (c1 == '=') AOP(O_DIV, 2); TOK(T_SLASH);
    case '%': if (c1 == '=') AOP(O_MOD, 2); TOK(T_PERCENT);
    case '&': if (c1 == '&') TOK2(T_ANDAND); if (c1 == '=') AOP(O_BAND, 2); TOK(T_AMP);
    case '|': if (c1 == '|') TOK2(T_OROR); if (c1 == '=') AOP(O_BOR, 2); TOK(T_PIPE);
    case '^': if (c1 == '=') AOP(O_BXOR, 2); TOK(T_CARET);
    case '!': if (c1 == '=') { if (c2 == '=') TOK3(T_SNE); TOK2(T_NE); } TOK(T_BANG);
    case '=': if (c1 == '=') { if (c2 == '=') TOK3(T_SEQ); TOK2(T_EQ); } if (c1 == '>') TOK2(T_ARROW); TOK(T_ASSIGN);
    case '<': if (c1 == '<') { if (c2 == '=') AOP(O_SHL, 3); TOK2(T_SHL); } if (c1 == '=') TOK2(T_LE); TOK(T_LT);
    case '>':
        if (c1 == '>') {
            if (c2 == '>') { if (c3 == '=') AOP(O_USHR, 4); TOK3(T_USHR); }
            if (c2 == '=') AOP(O_SHR, 3);
            TOK2(T_SHR);
        }
        if (c1 == '=') TOK2(T_GE);
        TOK(T_GT);
    }
    perr(p, "Invalid or unexpected token");
    t->type = T_EOF;
}

static void next(parser *p) { lex(p); }

/* ---- AST helpers ------------------------------------------------------------------------------------------- */

static node *mk(parser *p, int type)
{
    node *n = (node *)arena_alloc(p->rt, sizeof(node));
    if (!n) { perr(p, "out of memory"); static node dummy; return &dummy; }
    n->type = (unsigned char)type;
    n->line = p->tok.line;
    return n;
}

static int accept(parser *p, int t)
{
    if (p->tok.type == t) { next(p); return 1; }
    return 0;
}

static int expect(parser *p, int t, const char *what)
{
    if (p->tok.type == t) { next(p); return 1; }
    perr(p, what);
    return 0;
}

static void semi(parser *p)
{
    if (p->tok.type == T_SEMI) { next(p); return; }
    if (p->tok.type == T_RBRACE || p->tok.type == T_EOF || p->tok.nl_before) return;
    perr(p, "Unexpected token");
}

static node *parse_assign(parser *p);
static node *parse_expr(parser *p);
static node *parse_stmt(parser *p);
static node *parse_function(parser *p, int is_decl, int arrow_start);

static int is_name_token(int t) { return t == T_IDENT || (t >= T_VAR && t <= T_DEFAULT); }

static void add_var(parser *p, jstr *name)
{
    for (node *v = p->fn->vars; v; v = v->next) if (v->str == name) return;
    node *v = mk(p, N_IDENT);
    v->str = name;
    v->next = p->fn->vars;
    p->fn->vars = v;
}

/* ---- expressions ------------------------------------------------------------------------------------------- */

static node *parse_template(parser *p, int start, int end);

static node *parse_args(parser *p)             /* after '(' ; returns the list, consumes ')' */
{
    node *head = NULL, **tail = &head;
    while (p->tok.type != T_RPAREN && !p->failed) {
        node *a;
        if (p->tok.type == T_ELLIPSIS) { next(p); a = mk(p, N_SPREAD); a->a = parse_assign(p); }
        else a = parse_assign(p);
        if (p->failed) return NULL;
        *tail = a; tail = &a->next;
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "Expected ')' after arguments");
    return head;
}

static node *parse_object(parser *p)
{
    node *o = mk(p, N_OBJECT);
    node **tail = &o->a;
    next(p);                                                    /* { */
    while (p->tok.type != T_RBRACE && !p->failed) {
        node *pr = mk(p, N_PROP);
        if (p->tok.type == T_ELLIPSIS) { perr(p, "Object spread is not supported"); return NULL; }
        if (p->tok.type == T_LBRACK) {                          /* computed key */
            next(p);
            pr->a = parse_assign(p);
            expect(p, T_RBRACK, "Expected ']'");
        } else if (p->tok.type == T_STR || is_name_token(p->tok.type)) {
            pr->str = p->tok.str;
            next(p);
        } else if (p->tok.type == T_NUM) {
            char b[40]; num_fmt(p->tok.num, b);
            pr->str = atom_c(p->rt, b);
            next(p);
        } else { perr(p, "Unexpected token in object literal"); return NULL; }
        if (p->tok.type == T_COLON) { next(p); pr->b = parse_assign(p); }
        else if (p->tok.type == T_LPAREN) {                     /* method shorthand */
            jstr *name = pr->str;
            node *f = parse_function(p, 0, 1);
            if (f) f->str = name;
            pr->b = f;
        } else if (pr->str && !pr->a) {                         /* shorthand {a} */
            node *id = mk(p, N_IDENT); id->str = pr->str; pr->b = id;
        } else { perr(p, "Expected ':' in object literal"); return NULL; }
        if (p->failed) return NULL;
        *tail = pr; tail = &pr->next;
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RBRACE, "Expected '}'");
    return o;
}

static node *parse_array(parser *p)
{
    node *arr = mk(p, N_ARRAY);
    node **tail = &arr->a;
    next(p);                                                    /* [ */
    while (p->tok.type != T_RBRACK && !p->failed) {
        node *e;
        if (p->tok.type == T_COMMA) { e = mk(p, N_IDENT); e->str = p->rt->a_undefined; next(p); *tail = e; tail = &e->next; continue; }
        if (p->tok.type == T_ELLIPSIS) { next(p); e = mk(p, N_SPREAD); e->a = parse_assign(p); }
        else e = parse_assign(p);
        if (p->failed) return NULL;
        *tail = e; tail = &e->next;
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RBRACK, "Expected ']'");
    return arr;
}

/* Is the '(' at the current position the start of an arrow function parameter list? */
static int arrow_ahead(parser *p)
{
    const char *s = p->s; int len = p->len, pos = p->pos, line = p->line;
    token save = p->tok;
    int depth = 0, result = 0, fail0 = p->failed;
    for (;;) {
        if (p->tok.type == T_LPAREN) depth++;
        else if (p->tok.type == T_RPAREN) { depth--; if (depth == 0) { next(p); result = p->tok.type == T_ARROW; break; } }
        else if (p->tok.type == T_EOF) break;
        next(p);
        if (p->failed && !fail0) break;
    }
    p->failed = fail0;
    p->s = s; p->len = len; p->pos = pos; p->line = line; p->tok = save;
    return result;
}

static node *parse_primary(parser *p)
{
    token t = p->tok;
    node *n;
    switch (t.type) {
    case T_NUM: n = mk(p, N_NUM); n->num = t.num; next(p); return n;
    case T_STR: n = mk(p, N_STR); n->str = t.str; next(p); return n;
    case T_TMPL: { next(p); return parse_template(p, t.start, t.end); }
    case T_THIS: n = mk(p, N_THIS); next(p); return n;
    case T_NULL: n = mk(p, N_NULL); next(p); return n;
    case T_TRUE: n = mk(p, N_TRUE); next(p); return n;
    case T_FALSE: n = mk(p, N_FALSE); next(p); return n;
    case T_FUNCTION: return parse_function(p, 0, 0);
    case T_LBRACK: return parse_array(p);
    case T_LBRACE: return parse_object(p);
    case T_LPAREN: {
        if (arrow_ahead(p)) return parse_function(p, 0, 2);
        next(p);
        int save_in = p->no_in; p->no_in = 0;
        n = parse_expr(p);
        p->no_in = save_in;
        expect(p, T_RPAREN, "Expected ')'");
        return n;
    }
    case T_IDENT: {
        n = mk(p, N_IDENT);
        n->str = t.str;
        next(p);
        if (p->tok.type == T_ARROW && !p->tok.nl_before) {      /* x => ... */
            node *id = n;
            node *f = parse_function(p, 0, 3);
            if (f) { f->params = id; f->nparams = 1; }
            return f;
        }
        if (n->str == p->rt->a_arguments) p->fn->uses_args = 1;
        return n;
    }
    default:
        perr(p, "Unexpected token");
        return NULL;
    }
}

static node *parse_lhs(parser *p)
{
    node *e;
    if (p->tok.type == T_NEW) {
        next(p);
        node *n = mk(p, N_NEW);
        node *callee = p->tok.type == T_NEW ? parse_lhs(p) : parse_primary(p);
        if (p->failed) return NULL;
        for (;;) {                                              /* member accesses only, no calls */
            if (p->tok.type == T_DOT) {
                next(p);
                if (!is_name_token(p->tok.type)) { perr(p, "Expected property name"); return NULL; }
                node *m = mk(p, N_MEMBER); m->a = callee; m->str = p->tok.str; callee = m; next(p);
            } else if (p->tok.type == T_LBRACK) {
                next(p);
                node *m = mk(p, N_INDEX); m->a = callee; m->b = parse_expr(p); callee = m;
                expect(p, T_RBRACK, "Expected ']'");
            } else break;
        }
        n->a = callee;
        if (accept(p, T_LPAREN)) n->b = parse_args(p);
        e = n;
    } else e = parse_primary(p);
    for (;;) {
        if (p->failed || !e) return NULL;
        if (p->tok.type == T_DOT) {
            next(p);
            if (!is_name_token(p->tok.type)) { perr(p, "Expected property name"); return NULL; }
            node *m = mk(p, N_MEMBER); m->a = e; m->str = p->tok.str; e = m; next(p);
        } else if (p->tok.type == T_LBRACK) {
            next(p);
            node *m = mk(p, N_INDEX); m->a = e;
            int save_in = p->no_in; p->no_in = 0;
            m->b = parse_expr(p);
            p->no_in = save_in;
            e = m;
            expect(p, T_RBRACK, "Expected ']'");
        } else if (p->tok.type == T_LPAREN) {
            next(p);
            node *c = mk(p, N_CALL); c->a = e;
            int save_in = p->no_in; p->no_in = 0;
            c->b = parse_args(p);
            p->no_in = save_in;
            e = c;
        } else break;
    }
    return e;
}

static node *parse_postfix(parser *p)
{
    node *e = parse_lhs(p);
    if (p->failed || !e) return NULL;
    if ((p->tok.type == T_INC || p->tok.type == T_DEC) && !p->tok.nl_before) {
        if (e->type != N_IDENT && e->type != N_MEMBER && e->type != N_INDEX) { perr(p, "Invalid left-hand side expression in postfix operation"); return NULL; }
        node *u = mk(p, N_UPDATE);
        u->op = (unsigned char)(p->tok.type == T_INC ? O_INC : O_DEC);
        u->flag = 0;
        u->a = e;
        next(p);
        return u;
    }
    return e;
}

static node *parse_unary(parser *p)
{
    int op = 0, t = p->tok.type;
    switch (t) {
    case T_BANG: op = O_NOT; break;
    case T_TILDE: op = O_BNOT; break;
    case T_PLUS: op = O_PLUS; break;
    case T_MINUS: op = O_MINUS; break;
    case T_TYPEOF: op = O_TYPEOF; break;
    case T_VOID: op = O_VOID; break;
    case T_DELETE: op = O_DELETE; break;
    case T_INC: case T_DEC: {
        node *u = mk(p, N_UPDATE);
        u->op = (unsigned char)(t == T_INC ? O_INC : O_DEC);
        u->flag = 1;
        next(p);
        if (++p->depth > MAX_PARSE_DEPTH) { perr(p, "Expression nested too deeply"); return NULL; }
        u->a = parse_unary(p);
        p->depth--;
        if (!u->a) return NULL;
        if (u->a->type != N_IDENT && u->a->type != N_MEMBER && u->a->type != N_INDEX) { perr(p, "Invalid left-hand side expression in prefix operation"); return NULL; }
        return u;
    }
    default: return parse_postfix(p);
    }
    node *u = mk(p, N_UNARY);
    u->op = (unsigned char)op;
    next(p);
    if (++p->depth > MAX_PARSE_DEPTH) { perr(p, "Expression nested too deeply"); return NULL; }
    u->a = parse_unary(p);
    p->depth--;
    return u->a ? u : NULL;
}

static int binop_info(parser *p, int *prec, int *op, int *logical)
{
    *logical = 0;
    switch (p->tok.type) {
    case T_QQ: *prec = 1; *op = O_NULLISH; *logical = 1; return 1;
    case T_OROR: *prec = 2; *op = O_OR; *logical = 1; return 1;
    case T_ANDAND: *prec = 3; *op = O_AND; *logical = 1; return 1;
    case T_PIPE: *prec = 4; *op = O_BOR; return 1;
    case T_CARET: *prec = 5; *op = O_BXOR; return 1;
    case T_AMP: *prec = 6; *op = O_BAND; return 1;
    case T_EQ: *prec = 7; *op = O_EQ; return 1;
    case T_NE: *prec = 7; *op = O_NE; return 1;
    case T_SEQ: *prec = 7; *op = O_SEQ; return 1;
    case T_SNE: *prec = 7; *op = O_SNE; return 1;
    case T_LT: *prec = 8; *op = O_LT; return 1;
    case T_GT: *prec = 8; *op = O_GT; return 1;
    case T_LE: *prec = 8; *op = O_LE; return 1;
    case T_GE: *prec = 8; *op = O_GE; return 1;
    case T_INSTANCEOF: *prec = 8; *op = O_INSTANCEOF; return 1;
    case T_IN: if (p->no_in) return 0; *prec = 8; *op = O_IN; return 1;
    case T_SHL: *prec = 9; *op = O_SHL; return 1;
    case T_SHR: *prec = 9; *op = O_SHR; return 1;
    case T_USHR: *prec = 9; *op = O_USHR; return 1;
    case T_PLUS: *prec = 10; *op = O_ADD; return 1;
    case T_MINUS: *prec = 10; *op = O_SUB; return 1;
    case T_STAR: *prec = 11; *op = O_MUL; return 1;
    case T_SLASH: *prec = 11; *op = O_DIV; return 1;
    case T_PERCENT: *prec = 11; *op = O_MOD; return 1;
    case T_STARSTAR: *prec = 12; *op = O_POW; return 1;
    }
    return 0;
}

static node *parse_binary(parser *p, int minprec)
{
    node *left = parse_unary(p);
    if (!left) return NULL;
    for (;;) {
        int prec, op, logical;
        if (!binop_info(p, &prec, &op, &logical) || prec < minprec) break;
        next(p);
        node *right = parse_binary(p, op == O_POW ? prec : prec + 1);   /* ** is right associative */
        if (!right) return NULL;
        node *b = mk(p, logical ? N_LOGICAL : N_BINARY);
        b->op = (unsigned char)op;
        b->a = left; b->b = right;
        left = b;
    }
    return left;
}

static node *parse_assign(parser *p)
{
    if (p->failed) return NULL;
    if (++p->depth > MAX_PARSE_DEPTH) { perr(p, "Expression nested too deeply"); return NULL; }
    node *left = parse_binary(p, 0);
    if (!left) { p->depth--; return NULL; }
    if (p->tok.type == T_QUESTION) {
        next(p);
        node *c = mk(p, N_COND);
        c->a = left;
        int save_in = p->no_in; p->no_in = 0;
        c->b = parse_assign(p);
        p->no_in = save_in;
        expect(p, T_COLON, "Expected ':' in conditional expression");
        c->c = parse_assign(p);
        p->depth--;
        return (c->b && c->c) ? c : NULL;
    }
    if (p->tok.type == T_ASSIGN || p->tok.type == T_ASSIGN_OP) {
        if (left->type != N_IDENT && left->type != N_MEMBER && left->type != N_INDEX) { perr(p, "Invalid left-hand side in assignment"); p->depth--; return NULL; }
        node *a = mk(p, N_ASSIGN);
        a->op = (unsigned char)(p->tok.type == T_ASSIGN ? O_ASSIGN : p->tok.op);
        next(p);
        a->a = left;
        a->b = parse_assign(p);
        p->depth--;
        return a->b ? a : NULL;
    }
    p->depth--;
    return left;
}

static node *parse_expr(parser *p)
{
    node *e = parse_assign(p);
    while (e && p->tok.type == T_COMMA) {
        next(p);
        node *s = mk(p, N_SEQ);
        s->a = e;
        s->b = parse_assign(p);
        if (!s->b) return NULL;
        e = s;
    }
    return e;
}

/* `text ${expr} text` -> N_TMPL with a list of N_STR and expression nodes */
static node *parse_template(parser *p, int start, int end)
{
    node *t = mk(p, N_TMPL);
    node **tail = &t->a;
    const char *s = p->s;
    int i = start;
    sbuf b; sb_init(&b);
    while (i < end && !p->failed) {
        if (s[i] == '\\') { i++; if (!read_escape(p, s, end, &i, &b, 1)) break; continue; }
        if (s[i] == '$' && i + 1 < end && s[i + 1] == '{') {
            node *str = mk(p, N_STR);
            str->str = atom_new(p->rt, b.p ? b.p : "", b.len);
            b.len = 0; if (b.p) b.p[0] = 0;
            *tail = str; tail = &str->next;
            int depth = 1, j = i + 2;
            while (j < end && depth) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') { depth--; if (!depth) break; }
                else if (s[j] == '\'' || s[j] == '"') { int q = s[j++]; while (j < end && s[j] != q) { if (s[j] == '\\') j++; j++; } }
                else if (s[j] == '`') { j++; int d2 = 0; while (j < end && !(s[j] == '`' && !d2)) { if (s[j] == '\\') { j += 2; continue; } if (s[j] == '$' && j + 1 < end && s[j + 1] == '{') d2++; else if (s[j] == '}' && d2) d2--; j++; } }
                j++;
            }
            if (depth) { perr(p, "Unterminated template expression"); break; }
            /* parse s[i+2 .. j) as an expression */
            const char *os = p->s; int olen = p->len, opos = p->pos, oline = p->line, ofail_depth = p->depth;
            token otok = p->tok;
            p->s = s; p->len = j; p->pos = i + 2;
            next(p);
            node *e = parse_expr(p);
            if (!p->failed && p->tok.type != T_EOF) perr(p, "Unexpected token in template expression");
            p->s = os; p->len = olen; p->pos = opos; p->line = oline; p->tok = otok; p->depth = ofail_depth;
            if (!e) break;
            *tail = e; tail = &e->next;
            i = j + 1;
            continue;
        }
        if (s[i] == '\r' && i + 1 < end && s[i + 1] == '\n') i++;      /* CRLF becomes LF */
        sb_addc(&b, s[i++]);
    }
    node *str = mk(p, N_STR);
    str->str = atom_new(p->rt, b.p ? b.p : "", b.len);
    *tail = str;
    sb_free(&b);
    return p->failed ? NULL : t;
}

/* ---- functions --------------------------------------------------------------------------------------------- */

/* mode: 0 = function keyword, 1 = method shorthand (at '('), 2 = arrow with ( params ), 3 = arrow with single
 * identifier (current token is '=>'). */
static node *parse_function(parser *p, int is_decl, int mode)
{
    node *f = mk(p, N_FUNC);
    node *outer = p->fn;
    if (mode == 0) {
        next(p);                                                /* function */
        if (is_name_token(p->tok.type)) { f->str = p->tok.str; next(p); }
        else if (is_decl) { perr(p, "Function statements require a function name"); return NULL; }
    }
    if (mode == 2 || mode == 3) f->flag = 1;                    /* arrow */
    p->fn = f;
    if (mode != 3) {
        if (!expect(p, T_LPAREN, "Expected '(' before parameters")) { p->fn = outer; return NULL; }
        node **tail = &f->params;
        while (p->tok.type != T_RPAREN && !p->failed) {
            if (!is_name_token(p->tok.type)) { perr(p, "Invalid parameter"); p->fn = outer; return NULL; }
            node *id = mk(p, N_IDENT); id->str = p->tok.str;
            next(p);
            if (p->tok.type == T_ASSIGN) { next(p); id->a = parse_assign(p); }   /* default value */
            *tail = id; tail = &id->next; f->nparams++;
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RPAREN, "Expected ')' after parameters");
    }
    if (mode == 2 || mode == 3) {
        if (!expect(p, T_ARROW, "Expected '=>'")) { p->fn = outer; return NULL; }
    }
    int save_in = p->no_in; p->no_in = 0;
    if ((mode == 2 || mode == 3) && p->tok.type != T_LBRACE) {
        f->expr_body = 1;
        f->body = parse_assign(p);
    } else {
        if (!expect(p, T_LBRACE, "Expected '{' before function body")) { p->fn = outer; p->no_in = save_in; return NULL; }
        node **tail = &f->body;
        while (p->tok.type != T_RBRACE && p->tok.type != T_EOF && !p->failed) {
            node *s = parse_stmt(p);
            if (!s) break;
            *tail = s; tail = &s->next;
        }
        expect(p, T_RBRACE, "Expected '}' at end of function body");
    }
    p->no_in = save_in;
    p->fn = outer;
    if (f->uses_args && f->flag && outer) outer->uses_args = 1;  /* arrows see the enclosing arguments */
    if (p->failed) return NULL;
    return f;
}

/* ---- statements -------------------------------------------------------------------------------------------- */

static node *parse_var(parser *p, int kind)             /* current token is var/let/const */
{
    node *v = mk(p, N_VAR);
    v->flag = (unsigned char)kind;
    next(p);
    node **tail = &v->a;
    for (;;) {
        if (!is_name_token(p->tok.type)) { perr(p, "Expected variable name"); return NULL; }
        node *d = mk(p, N_DECL);
        d->str = p->tok.str;
        next(p);
        if (kind == VK_VAR) add_var(p, d->str);
        else p->block_lex = 1;
        if (accept(p, T_ASSIGN)) { d->a = parse_assign(p); if (!d->a) return NULL; }
        else if (kind == VK_CONST && p->tok.type != T_IN && !(p->tok.type == T_IDENT && p->tok.str->len == 2 && p->tok.str->s[0] == 'o' && p->tok.str->s[1] == 'f')) { perr(p, "Missing initializer in const declaration"); return NULL; }
        *tail = d; tail = &d->next;
        if (!accept(p, T_COMMA)) break;
    }
    return v;
}

static node *parse_block(parser *p)
{
    node *b = mk(p, N_BLOCK);
    int save = p->block_lex;
    p->block_lex = 0;
    next(p);                                                    /* { */
    node **tail = &b->a;
    while (p->tok.type != T_RBRACE && p->tok.type != T_EOF && !p->failed) {
        node *s = parse_stmt(p);
        if (!s) break;
        *tail = s; tail = &s->next;
    }
    expect(p, T_RBRACE, "Expected '}'");
    b->flag = (unsigned char)p->block_lex;
    p->block_lex = save;
    return p->failed ? NULL : b;
}

static int is_of(parser *p) { return p->tok.type == T_IDENT && p->tok.str->len == 2 && p->tok.str->s[0] == 'o' && p->tok.str->s[1] == 'f'; }

static node *parse_for(parser *p)
{
    node *f = mk(p, N_FOR);
    next(p);
    if (!expect(p, T_LPAREN, "Expected '(' after for")) return NULL;
    int save_lex = p->block_lex;
    node *init = NULL;
    if (p->tok.type == T_SEMI) { /* no init */ }
    else {
        p->no_in = 1;
        if (p->tok.type == T_VAR || p->tok.type == T_LET || p->tok.type == T_CONST)
            init = parse_var(p, p->tok.type == T_VAR ? VK_VAR : p->tok.type == T_LET ? VK_LET : VK_CONST);
        else init = parse_expr(p);
        p->no_in = 0;
        if (!init) return NULL;
    }
    if (p->tok.type == T_IN || is_of(p)) {                      /* for (x in obj) / for (x of arr) */
        int of = p->tok.type != T_IN;
        f->type = (unsigned char)(of ? N_FOROF : N_FORIN);
        next(p);
        f->a = init;
        f->b = parse_expr(p);
        expect(p, T_RPAREN, "Expected ')'");
        f->c = parse_stmt(p);
        f->flag = (unsigned char)(init && init->type == N_VAR && init->flag != VK_VAR);
        p->block_lex = save_lex;
        return p->failed ? NULL : f;
    }
    f->a = init;
    f->flag = (unsigned char)(init && init->type == N_VAR && init->flag != VK_VAR);
    if (!expect(p, T_SEMI, "Expected ';' in for statement")) return NULL;
    if (p->tok.type != T_SEMI) f->b = parse_expr(p);
    if (!expect(p, T_SEMI, "Expected ';' in for statement")) return NULL;
    if (p->tok.type != T_RPAREN) f->c = parse_expr(p);
    expect(p, T_RPAREN, "Expected ')'");
    f->d = parse_stmt(p);
    p->block_lex = save_lex;
    return p->failed ? NULL : f;
}

static node *parse_stmt(parser *p)
{
    if (p->failed) return NULL;
    if (++p->depth > MAX_PARSE_DEPTH) { perr(p, "Statements nested too deeply"); return NULL; }
    node *r = NULL;
    switch (p->tok.type) {
    case T_LBRACE: r = parse_block(p); break;
    case T_SEMI: r = mk(p, N_EMPTY); next(p); break;
    case T_VAR: case T_LET: case T_CONST:
        r = parse_var(p, p->tok.type == T_VAR ? VK_VAR : p->tok.type == T_LET ? VK_LET : VK_CONST);
        if (r) semi(p);
        break;
    case T_FUNCTION: {
        node *f = parse_function(p, 1, 0);
        if (!f) break;
        /* hoist: functions are defined on entry to the enclosing function */
        node *h = mk(p, N_FUNCDECL); h->a = f; h->str = f->str; h->next = p->fn->funcs; p->fn->funcs = h;
        r = mk(p, N_EMPTY);
        break;
    }
    case T_IF: {
        r = mk(p, N_IF);
        next(p);
        expect(p, T_LPAREN, "Expected '(' after if");
        r->a = parse_expr(p);
        expect(p, T_RPAREN, "Expected ')'");
        r->b = parse_stmt(p);
        if (accept(p, T_ELSE)) r->c = parse_stmt(p);
        if (p->failed) r = NULL;
        break;
    }
    case T_FOR: r = parse_for(p); break;
    case T_WHILE: {
        r = mk(p, N_WHILE);
        next(p);
        expect(p, T_LPAREN, "Expected '(' after while");
        r->a = parse_expr(p);
        expect(p, T_RPAREN, "Expected ')'");
        r->b = parse_stmt(p);
        if (p->failed) r = NULL;
        break;
    }
    case T_DO: {
        r = mk(p, N_DOWHILE);
        next(p);
        r->b = parse_stmt(p);
        expect(p, T_WHILE, "Expected 'while' after do body");
        expect(p, T_LPAREN, "Expected '('");
        r->a = parse_expr(p);
        expect(p, T_RPAREN, "Expected ')'");
        accept(p, T_SEMI);
        if (p->failed) r = NULL;
        break;
    }
    case T_RETURN: {
        r = mk(p, N_RETURN);
        next(p);
        if (p->tok.type != T_SEMI && p->tok.type != T_RBRACE && p->tok.type != T_EOF && !p->tok.nl_before) r->a = parse_expr(p);
        semi(p);
        if (p->failed) r = NULL;
        break;
    }
    case T_BREAK: r = mk(p, N_BREAK); next(p); semi(p); break;
    case T_CONTINUE: r = mk(p, N_CONTINUE); next(p); semi(p); break;
    case T_THROW: {
        r = mk(p, N_THROW);
        next(p);
        if (p->tok.nl_before) { perr(p, "Illegal newline after throw"); break; }
        r->a = parse_expr(p);
        semi(p);
        if (p->failed) r = NULL;
        break;
    }
    case T_TRY: {
        r = mk(p, N_TRY);
        next(p);
        if (p->tok.type != T_LBRACE) { perr(p, "Expected '{' after try"); break; }
        r->a = parse_block(p);
        if (accept(p, T_CATCH)) {
            node *c = mk(p, N_CASE);
            if (accept(p, T_LPAREN)) {
                if (!is_name_token(p->tok.type)) { perr(p, "Expected catch parameter"); break; }
                c->str = p->tok.str; next(p);
                expect(p, T_RPAREN, "Expected ')'");
            }
            if (p->tok.type != T_LBRACE) { perr(p, "Expected '{' after catch"); break; }
            c->b = parse_block(p);
            r->b = c;
        }
        if (accept(p, T_FINALLY)) {
            if (p->tok.type != T_LBRACE) { perr(p, "Expected '{' after finally"); break; }
            r->c = parse_block(p);
        }
        if (!r->b && !r->c) perr(p, "Missing catch or finally after try");
        if (p->failed) r = NULL;
        break;
    }
    case T_SWITCH: {
        r = mk(p, N_SWITCH);
        next(p);
        expect(p, T_LPAREN, "Expected '(' after switch");
        r->a = parse_expr(p);
        expect(p, T_RPAREN, "Expected ')'");
        expect(p, T_LBRACE, "Expected '{'");
        node **ct = &r->b;
        int save = p->block_lex; p->block_lex = 0;
        while (p->tok.type != T_RBRACE && p->tok.type != T_EOF && !p->failed) {
            node *c = mk(p, N_CASE);
            if (accept(p, T_CASE)) c->a = parse_expr(p);
            else if (accept(p, T_DEFAULT)) c->flag = 1;
            else { perr(p, "Expected 'case' or 'default'"); break; }
            expect(p, T_COLON, "Expected ':'");
            node **st = &c->b;
            while (p->tok.type != T_CASE && p->tok.type != T_DEFAULT && p->tok.type != T_RBRACE && p->tok.type != T_EOF && !p->failed) {
                node *s = parse_stmt(p);
                if (!s) break;
                *st = s; st = &s->next;
            }
            *ct = c; ct = &c->next;
        }
        expect(p, T_RBRACE, "Expected '}'");
        r->flag = (unsigned char)p->block_lex;
        p->block_lex = save;
        if (p->failed) r = NULL;
        break;
    }
    default: {
        r = mk(p, N_EXPR);
        r->a = parse_expr(p);
        if (!r->a) { r = NULL; break; }
        semi(p);
        if (p->failed) r = NULL;
    }
    }
    p->depth--;
    return p->failed ? NULL : r;
}

node *js_parse(js_rt *rt, const char *src, int len, char *err, int errmax)
{
    parser p;
    memset(&p, 0, sizeof p);
    p.rt = rt; p.s = src; p.len = len; p.line = 1; p.err = err; p.errmax = errmax;
    if (err && errmax > 0) err[0] = 0;
    node *prog = (node *)arena_alloc(rt, sizeof(node));
    if (!prog) return NULL;
    prog->type = N_FUNC;
    prog->line = 1;
    p.fn = prog;
    next(&p);
    node **tail = &prog->body;
    while (p.tok.type != T_EOF && !p.failed) {
        node *s = parse_stmt(&p);
        if (!s) break;
        *tail = s; tail = &s->next;
    }
    if (p.failed) return NULL;
    return prog;
}
