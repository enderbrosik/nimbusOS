/* NimbusOS - domrun: loads an HTML page into the JavaScript engine's DOM layer, runs its scripts and then plays a
 * list of actions, for the tests (host side).
 *   domrun page.html [action ...]
 * actions:  click=SELECTOR  type=SELECTOR:text  check=SELECTOR  submit=SELECTOR  advance=MS  dump  log=text */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "js/dom_int.h"

static long long g_now = 1000000;
static void h_log(void *ud, const char *s, int len) { (void)ud; printf("LOG: %.*s\n", len, s); }
static void h_nav(void *ud, const char *u) { (void)ud; printf("NAV: %s\n", u); }
static long long h_now(void *ud) { (void)ud; return g_now; }

static dnode *find(dompage *d, const char *sel)
{
    dnode *f = NULL;
    sel_query(d, d->doc, sel, 0, NULL, &f);
    if (!f) printf("ERROR: nothing matches %s\n", sel);
    return f;
}

static void report(const char *what, int r) { printf("%s -> %s%s%s\n", what, (r & DOM_CHANGED) ? "changed " : "", (r & DOM_PREVENTED) ? "prevented " : "", (r & DOM_NAVIGATE) ? "navigate" : ""); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: domrun page.html [action ...]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *html = malloc((size_t)len + 1);
    if (fread(html, 1, (size_t)len, f) != (size_t)len) return 2;
    html[len] = 0; fclose(f);
    struct dom_host host = { h_log, h_nav, h_now, NULL };
    dompage *d = dom_new(html, "http://example.test/dir/page.html?x=1#top", &host);
    if (!d) { fprintf(stderr, "dom_new failed\n"); return 2; }
    if (getenv("JS_GC_STRESS")) d->rt->gc_stress = (unsigned)atoi(getenv("JS_GC_STRESS"));
    report("scripts", dom_run_scripts(d));
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "click=", 6)) {
            dnode *n = find(d, a + 6);
            if (n) { char nav[256] = ""; int r = dom_click(d, n->id, nav, sizeof nav); report(a, r); if (nav[0]) printf("HREF: %s\n", nav); }
        } else if (!strncmp(a, "type=", 5)) {
            char sel[100]; const char *c = strchr(a + 5, ':');
            if (!c) continue;
            snprintf(sel, sizeof sel, "%.*s", (int)(c - (a + 5)), a + 5);
            dnode *n = find(d, sel);
            if (n) { dom_set_field(d, n->id, c + 1, 0); report(a, dom_input(d, n->id)); }
        } else if (!strncmp(a, "check=", 6)) {
            dnode *n = find(d, a + 6);
            if (n) { dom_set_field(d, n->id, NULL, 1); report(a, dom_click(d, n->id, NULL, 0)); }
        } else if (!strncmp(a, "submit=", 7)) {
            dnode *n = find(d, a + 7);
            if (n) report(a, dom_submit(d, n->id));
        } else if (!strncmp(a, "advance=", 8)) {
            g_now += atoll(a + 8);
            report(a, dom_poll(d));
        } else if (!strcmp(a, "dump")) {
            int l = 0; char *s = dom_serialize(d, &l);
            printf("HTML: %.*s\n", l, s ? s : "");
            dom_str_free(s);
        } else if (!strncmp(a, "log=", 4)) printf("-- %s\n", a + 4);
    }
    printf("timers: %s\n", dom_has_timers(d) ? "yes" : "no");
    dom_free(d);
    free(html);
    return 0;
}
