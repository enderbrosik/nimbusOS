/* spin [seconds] - burns CPU in pure ring 3 code (no system calls in the
 * inner loop) and reports progress.  The desktop stays responsive because
 * the timer interrupt preempts user programs; Ctrl+C stops it at any time. */
#include <stdio.h>
#include <stdlib.h>
#include <nimbus.h>

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    unsigned long long start = ticks_ms(), loops = 0;
    volatile unsigned long x = 1;
    for (int s = 1; s <= seconds; s++) {
        while (ticks_ms() - start < (unsigned long long)s * 1000) {
            for (int i = 0; i < 2000000; i++) x = x * 6364136223846793005ul + 1442695040888963407ul;
            loops++;
        }
        printf("%d s: %llu million iterations\n", s, loops * 2);
    }
    printf("done (%lx)\n", x);
    return 0;
}
