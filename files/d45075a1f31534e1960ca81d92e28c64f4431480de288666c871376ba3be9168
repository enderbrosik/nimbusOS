/* primes [limit] - sieve of Eratosthenes using malloc'ed memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nimbus.h>

int main(int argc, char **argv)
{
    long limit = argc > 1 ? strtol(argv[1], NULL, 10) : 1000000;
    if (limit < 2 || limit > 50000000) { fprintf(stderr, "usage: primes [2..50000000]\n"); return 1; }
    unsigned long long t0 = ticks_ms();
    char *composite = calloc(limit + 1, 1);
    if (!composite) { fprintf(stderr, "primes: out of memory\n"); return 1; }
    long count = 0, last[5] = { 0 };
    for (long i = 2; i <= limit; i++) {
        if (composite[i]) continue;
        last[count++ % 5] = i;
        for (long j = i * i; j <= limit; j += i) composite[j] = 1;
    }
    unsigned long long ms = ticks_ms() - t0;
    printf("%ld primes up to %ld (%llu ms, %ld KiB of heap)\n", count, limit, ms, (limit + 1) / 1024);
    printf("largest:");
    long m = count < 5 ? count : 5;
    for (long k = 0; k < m; k++) printf(" %ld", last[(count - m + k) % 5]);
    printf("\n");
    free(composite);
    return 0;
}
