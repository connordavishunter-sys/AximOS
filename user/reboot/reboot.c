#include <stdio.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: reboot\n"
                            "Restarts the computer.\n");

    reboot(RB_AUTOBOOT);
    printf("reboot: failed\n");
    return 1;
}
