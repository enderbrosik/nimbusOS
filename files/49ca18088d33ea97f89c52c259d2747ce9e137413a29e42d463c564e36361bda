/* todo [add <text> | done <n> | clear] - a to-do list kept in
 * /Documents/todo.txt (demonstrates reading and writing files). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nimbus.h>

#define FILE_PATH "/Documents/todo.txt"

static char *load(long *len)
{
    long n = readfile(FILE_PATH, NULL, 0);
    if (n < 0) n = 0;
    char *b = malloc(n + 1);
    if (n) readfile(FILE_PATH, b, n);
    b[n] = 0;
    *len = n;
    return b;
}

int main(int argc, char **argv)
{
    long len;
    char *items = load(&len);
    if (argc >= 3 && !strcmp(argv[1], "add")) {
        char line[256] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strcat(line, " ");
            if (strlen(line) + strlen(argv[i]) < sizeof line - 2) strcat(line, argv[i]);
        }
        strcat(line, "\n");
        char *out = malloc(len + strlen(line) + 1);
        strcpy(out, items);
        strcat(out, line);
        int r = writefile(FILE_PATH, out, strlen(out));
        if (r < 0) { fprintf(stderr, "todo: %s\n", strerror(r)); return 1; }
        printf("Added: %s", line);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "done")) {
        int which = atoi(argv[2]), n = 0;
        char *out = malloc(len + 1), *o = out;
        for (char *l = strtok(items, "\n"); l; l = strtok(NULL, "\n"))
            if (++n != which) { strcpy(o, l); o += strlen(l); *o++ = '\n'; }
            else printf("Done: %s\n", l);
        *o = 0;
        if (which < 1 || which > n) { fprintf(stderr, "todo: no item %s\n", argv[2]); return 1; }
        writefile(FILE_PATH, out, strlen(out));
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "clear")) { writefile(FILE_PATH, "", 0); printf("List cleared.\n"); return 0; }
    if (argc != 1) { fprintf(stderr, "usage: todo [add <text> | done <n> | clear]\n"); return 1; }
    int n = 0;
    for (char *l = strtok(items, "\n"); l; l = strtok(NULL, "\n")) printf("%2d. %s\n", ++n, l);
    if (!n) printf("Nothing to do. Add an item with: todo add <text>\n");
    return 0;
}
