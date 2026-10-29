/* NimbusOS - jsrun: runs a JavaScript file with the engine in js/ (host-side, for the tests).
 *   jsrun script.js            prints console.log output; exit status 1 and the error on stderr if the script fails
 *   jsrun -e 'code'            runs the code given on the command line */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "js/js.h"

static void out(void *ud, const char *s, int len) { (void)ud; fwrite(s, 1, (size_t)len, stdout); }
static long long clk(void *ud) { (void)ud; struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

int main(int argc, char **argv)
{
    char *src = NULL; long len = 0;
    if (argc >= 3 && !strcmp(argv[1], "-e")) { src = strdup(argv[2]); len = (long)strlen(src); }
    else if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        src = malloc((size_t)len + 1);
        if (fread(src, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 2; }
        fclose(f);
    } else { fprintf(stderr, "usage: jsrun file.js | jsrun -e 'code'\n"); return 2; }
    js_rt *rt = js_new(out, NULL);
    if (!rt) { fprintf(stderr, "jsrun: could not create runtime\n"); return 2; }
    js_set_clock(rt, clk, NULL);
    char err[512];
    int r = js_eval(rt, src, (int)len, err, sizeof err);
    fflush(stdout);
    if (r) fprintf(stderr, "%s\n", err);
    js_free(rt);
    free(src);
    return r ? 1 : 0;
}
