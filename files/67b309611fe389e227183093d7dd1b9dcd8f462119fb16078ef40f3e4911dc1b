/* hello - the classic first program, plus its arguments and process id. */
#include <stdio.h>
#include <nimbus.h>

int main(int argc, char **argv)
{
    printf("Hello from user mode! I am process %d.\n", getpid());
    printf("I was started with %d argument%s:\n", argc, argc == 1 ? "" : "s");
    for (int i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
