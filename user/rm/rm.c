#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: rm file...\n"
                            "Removes files.\n");

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: rm file...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0) {
            dprintf(STDERR_FILENO, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
