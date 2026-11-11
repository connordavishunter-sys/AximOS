#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: echo [word...]\n"
                            "Prints the words, separated by spaces.\n");

    for (int i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? " " : "", argv[i]);
    printf("\n");
    return 0;
}
