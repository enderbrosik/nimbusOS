/* crash <how> - shows that a faulty program cannot take the system down.
 * The CPU raises an exception in ring 3; the kernel ends just this process
 * and reports why, the desktop and other programs keep running. */
#include <stdio.h>
#include <string.h>

static int recurse(int depth)
{
    volatile char frame[4096];          /* 4 KiB per call */
    frame[0] = (char)depth;
    if (depth < 0) return 0;            /* never true: keeps the compiler quiet */
    return recurse(depth + 1) + frame[0];
}

int main(int argc, char **argv)
{
    const char *how = argc > 1 ? argv[1] : "";
    if (!strcmp(how, "null")) {
        printf("Writing to a NULL pointer...\n");
        *(volatile int *)0 = 42;
    } else if (!strcmp(how, "kernel")) {
        printf("Writing into kernel memory at 0x100000 (supervisor-only page)...\n");
        *(volatile int *)0x100000 = 42;
    } else if (!strcmp(how, "div")) {
        int n = 7, zero = 0;
        printf("Dividing by zero...\n");
        fflush(stdout);
        /* a real idiv: in C, x / 0 is undefined and may be optimized away */
        __asm__ volatile("cltd\n\tidivl %2" : "+a"(n) : "d"(0), "r"(zero));
        printf("%d\n", n);
    } else if (!strcmp(how, "cli")) {
        printf("Executing a privileged instruction (cli)...\n");
        __asm__ volatile("cli");
    } else if (!strcmp(how, "stack")) {
        printf("Recursing until the 256 KiB stack overflows...\n");
        return recurse(1);
    } else {
        printf("usage: crash null | kernel | div | cli | stack\n");
        return 1;
    }
    printf("...this line is never reached.\n");
    return 0;
}
