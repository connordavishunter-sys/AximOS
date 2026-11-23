#include <aximos.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: clear\n"
                            "Clears the screen.\n");

    printf("\x1b[H\x1b[2J");
    return 0;
}
