/*
 * NimbusOS JavaScript engine - the DOM layer used by the web browser.
 *
 * The browser's own renderer works on HTML text, so a scripted page works like this: the page's HTML is parsed into
 * a tree here, the scripts run against that tree, and whenever it changed the tree is written back as HTML
 * (dom_serialize) and rendered by the browser as usual. Pages without <script> never come through here.
 */
#ifndef NIMBUS_DOM_H
#define NIMBUS_DOM_H

typedef struct dompage dompage;

struct dom_host {
    void (*log)(void *ud, const char *msg, int len);    /* console.log, alert, script errors (one message per call) */
    void (*navigate)(void *ud, const char *url);        /* location.href = ..., location.assign(...) */
    long long (*now_ms)(void *ud);                      /* milliseconds, any origin that only moves forward */
    void *ud;
};

/* Builds the tree for html. Returns NULL if out of memory. base_url is the page address. */
dompage *dom_new(const char *html, const char *base_url, const struct dom_host *host);
void     dom_free(dompage *d);

/* Runs the page's inline scripts, then DOMContentLoaded and load handlers. Returns 1 if the page changed. */
int      dom_run_scripts(dompage *d);

/* The current tree as HTML for the browser to render (malloc'ed; release with dom_str_free). */
char    *dom_serialize(dompage *d, int *len);
void     dom_str_free(char *s);

/* Events from the browser. Each returns a bit set: 1 = the page changed (render it again),
 * 2 = the default action was cancelled (preventDefault / return false). */
#define DOM_CHANGED   1
#define DOM_PREVENTED 2
#define DOM_NAVIGATE  4          /* dom_click: follow the link whose address was written to nav */
int      dom_click(dompage *d, int nid, char *nav, int navmax);
int      dom_input(dompage *d, int nid);                     /* the text of a field changed: input + change events */
int      dom_submit(dompage *d, int form_nid);               /* a form is about to be sent */

/* Tells the page what the user typed or ticked (no events fired). Call before dispatching any event. */
void     dom_set_field(dompage *d, int nid, const char *value, int checked);

/* Timers (setTimeout / setInterval). dom_poll runs what is due; returns DOM_CHANGED bits.
 * dom_has_timers tells the browser whether to keep polling. */
int      dom_poll(dompage *d);
int      dom_has_timers(dompage *d);

#endif
