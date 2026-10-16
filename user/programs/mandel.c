/* mandel - ASCII Mandelbrot set in 16.16 fixed point (no FPU needed).
 * It is pure computation: while it runs the desktop stays responsive
 * because the scheduler preempts user programs like any other task. */
#include <stdio.h>
#include <stdlib.h>
#include <nimbus.h>

#define FIX(x) ((long)((x) * 65536))

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 72, h = argc > 2 ? atoi(argv[2]) : 28;
    if (w < 10 || w > 200 || h < 5 || h > 100) { fprintf(stderr, "usage: mandel [width] [height]\n"); return 1; }
    const char *shades = " .:-=+*#%@";
    unsigned long long t0 = ticks_ms();
    long iters = 0;
    for (int y = 0; y < h; y++) {
        long ci = FIX(-1.2) + (FIX(2.4) * y) / (h - 1);
        for (int x = 0; x < w; x++) {
            long cr = FIX(-2.1) + (FIX(3.0) * x) / (w - 1);
            long zr = 0, zi = 0;
            int i = 0;
            for (; i < 200; i++) {
                long zr2 = (zr * zr) >> 16, zi2 = (zi * zi) >> 16;
                if (zr2 + zi2 > FIX(4)) break;
                zi = ((zr * zi) >> 15) + ci;
                zr = zr2 - zi2 + cr;
            }
            iters += i;
            putchar(i == 200 ? '@' : shades[i % 9]);
        }
        putchar('\n');
    }
    printf("%ld iterations in %llu ms\n", iters, ticks_ms() - t0);
    return 0;
}
