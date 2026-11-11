#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: rename old new\n"
                            "Renames a file or directory.\n");

    if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: rename old new\n");
        return 1;
    }

    if (rename(argv[1], argv[2]) < 0) {
        dprintf(STDERR_FILENO, "rename: %s to %s: %s\n", argv[1], argv[2], strerror(errno));
        return 1;
    }
    return 0;
}
