#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: env\n"
                            "Prints the environment variables.\n");

    for (char **e = environ; *e; e++)
        printf("%s\n", *e);
    return 0;
}
