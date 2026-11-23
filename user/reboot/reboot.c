#include <aximos.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// It's setuid root, so anyone with an account, as at the desktop, can ask init.

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: reboot\n"
                            "Restarts the computer, once init has asked every program to finish.\n");

    struct group *users = getgrnam("users");

    if (getuid() != 0 && (!users || getgid() != users->gr_gid)) {
        fprintf(stderr, "reboot: only root and those with accounts can\n");
        return 1;
    }
    if (kill(1, SIGTERM) < 0) {
        perror("reboot");
        return 1;
    }
    return 0;
}
