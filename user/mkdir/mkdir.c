#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: mkdir directory...\n"
                            "Creates directories.\n");

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: mkdir directory...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0) < 0) {
            dprintf(STDERR_FILENO, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
