#include <stdlib.h>
#define kmalloc(n) malloc(n)
#define kzalloc(n) calloc(1, (n))
#define krealloc(p, n) realloc((p), (n))
#define kfree(p) free(p)
