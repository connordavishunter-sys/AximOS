#include <stdio.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: shutdown\n"
                            "Turns the computer off.\n");

    reboot(RB_POWER_OFF);
    printf("shutdown: failed\n");
    return 1;
}
