/* guess - guess the number (reads lines from the Terminal). */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int secret = rand() % 100 + 1, tries = 0;
    char line[64];
    printf("I'm thinking of a number between 1 and 100.\n");
    for (;;) {
        printf("Your guess: ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) return 1;
        if (line[0] == 'q') { printf("The number was %d. Bye!\n", secret); return 0; }
        int g = atoi(line);
        tries++;
        if (g < 1 || g > 100) printf("Please type a number from 1 to 100 (or q to quit).\n");
        else if (g < secret) printf("Higher!\n");
        else if (g > secret) printf("Lower!\n");
        else { printf("Correct! You got it in %d tries.\n", tries); return 0; }
    }
}
