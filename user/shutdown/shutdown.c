#include <aximos.h>
#include <signal.h>
#include <stdio.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: shutdown\n"
                            "Turns off the computer, once init has asked every program to finish.\n");

    if (kill(1, SIGUSR2) < 0) {
        perror("shutdown");
        return 1;
    }
    return 0;
}
