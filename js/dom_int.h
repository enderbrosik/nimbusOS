/* NimbusOS JavaScript engine - DOM internals shared by dom_tree.c and dom_js.c. */
#ifndef DOM_INT_H
#define DOM_INT_H
#include "js/js_int.h"
#include "js/dom.h"

enum { DN_ELEMENT = 1, DN_TEXT = 3, DN_DOCUMENT = 9, DN_FRAGMENT = 11 };

typedef struct dattr { jstr *name; char *val; } dattr;        /* val: HTML-escaped text, as it appears in source */
typedef struct dlistener { jstr *type; jsval fn; } dlistener;

typedef struct dnode {
    int id;                                 /* 1-based index into dompage.nodes */
    unsigned char type;
    jstr *tag;                              /* lowercase atom; "#text", "#document", "#fragment" */
    dattr *attrs; int nattrs, acap;
    char *text; unsigned tlen;              /* text nodes: HTML-escaped text (script/style: raw source) */
    struct dnode *parent, *first, *last, *next, *prev;
    jsobj *wrapper;                         /* the JavaScript object for this node, once asked for */
    dlistener *ls; int nls, lcap;
} dnode;

struct dtimer { int id; int active; long long due, interval; };

struct dompage {
    js_rt *rt;
    struct dom_host host;
    char base[256];
    dnode **nodes; int nnodes, ncap;
    dnode *doc, *html, *head, *body;
    dnode *cur_script;                      /* the <script> being run (document.write inserts after it) */
    int dirty, ready;                       /* ready: 0 loading, 1 interactive, 2 complete */
    struct dtimer *timers; int ntimers, tcap, next_timer_id;
    jsobj *timer_fns;                       /* GC root: timer callbacks, same index as timers[] */
    jsobj *roots;                           /* GC root: listeners and anything else that must stay alive */
    jsobj *proto_node, *proto_style, *proto_classlist, *proto_event;
    jsobj *document, *location;
    dlistener *wls; int nwls, wcap;         /* window.addEventListener */
    jstr *a_n, *a_style, *a_classlist;
    int depth;                              /* nested event dispatches */
};

/* dom_tree.c */
dnode *dn_new(dompage *d, int type, jstr *tag);
dattr *dn_attr(dnode *n, jstr *name);
const char *dn_attr_raw(dnode *n, const char *name);      /* NULL when absent */
void   dn_set_attr_raw(dompage *d, dnode *n, jstr *name, const char *raw, unsigned len);
void   dn_del_attr(dompage *d, dnode *n, jstr *name);
void   dn_append(dompage *d, dnode *parent, dnode *child);
void   dn_insert_before(dompage *d, dnode *parent, dnode *child, dnode *ref);
void   dn_detach(dompage *d, dnode *n);
void   dn_clear_children(dompage *d, dnode *n);
void   dn_set_text(dompage *d, dnode *n, const char *raw, unsigned len);
void   dn_parse(dompage *d, dnode *parent, const char *html, unsigned len);
void   dn_serialize(dompage *d, dnode *n, sbuf *out, int inner_only, int for_render);
dnode *dn_clone(dompage *d, dnode *n, int deep);
void   dn_text_content(dnode *n, sbuf *plain);            /* decoded text of the subtree */
int    dn_is(dnode *n, const char *tag);
void   html_decode(const char *raw, unsigned len, sbuf *out);
void   html_encode(const char *s, unsigned len, sbuf *out, int attr);
int    dn_has_click(dompage *d, dnode *n);

int    sel_matches(dompage *d, dnode *n, const char *sel);
void   sel_query(dompage *d, dnode *root, const char *sel, int all, jsobj *out, dnode **first);
void   dn_by_tag(dompage *d, dnode *root, const char *tag, jsobj *out);      /* tag "*" = all */
void   dn_by_class(dompage *d, dnode *root, const char *cls, jsobj *out);
dnode *dn_by_id(dompage *d, dnode *root, const char *id);

/* dom_js.c */
jsval  dn_wrap(dompage *d, dnode *n);
void   dom_mark_dirty(dompage *d);

#endif
