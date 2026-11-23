#include <aximos.h>
#include <signal.h>
#include <stdio.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: reboot\n"
                            "Restarts the computer, once init has asked every program to finish.\n");

    if (kill(1, SIGTERM) < 0) {
        perror("reboot");
        return 1;
    }
    return 0;
}
