/*
 * NimbusOS JavaScript engine (ES5 core plus let/const, arrow functions, template literals and for..of).
 * Original code, no floating point: numbers are fixed point (see js_int.h).
 */
#ifndef NIMBUS_JS_H
#define NIMBUS_JS_H

typedef struct js_rt js_rt;
typedef void (*js_out_fn)(void *ud, const char *s, int len);   /* console.log / print output */

js_rt *js_new(js_out_fn out, void *ud);
void   js_free(js_rt *rt);

/* Safety limits (0 keeps the default): heap bytes, evaluation steps per js_eval call, call depth. */
void   js_set_limits(js_rt *rt, unsigned long max_bytes, unsigned long max_steps, int max_depth);

/* Clock for Date.now(): returns milliseconds since 1970-01-01 UTC. */
void   js_set_clock(js_rt *rt, long long (*fn)(void *ud), void *ud);

/* Runs a script in the global scope. Returns 0 on success; otherwise a message such as
 * "Uncaught TypeError: x is not a function" or "SyntaxError: ... (line 3)" is written to err. */
int    js_eval(js_rt *rt, const char *src, int len, char *err, int errmax);

/* Frees everything no longer reachable. Only call it when no script is running. */
void   js_gc(js_rt *rt);
void   js_set_gc_stress(js_rt *rt, unsigned every_n_allocs);   /* testing only: collect very often */
unsigned long js_mem_used(js_rt *rt);

#endif
