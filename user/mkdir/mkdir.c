#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define USAGE "usage: mkdir [-p] directory...\n"

// Makes each folder along the path in turn, those already there being fine.
static int make_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int failed = mkdir(path, 0) < 0 && errno != EEXIST;
        *p = '/';
        if (failed)
            return -1;
    }
    return mkdir(path, 0) < 0 && errno != EEXIST ? -1 : 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, USAGE "Creates directories. With -p, also the folders they're in, and\n"
                                  "it's no error if they're already there.\n");

    bool parents = argc > 1 && strcmp(argv[1], "-p") == 0;
    int first = parents ? 2 : 1, status = 0;

    if (argc <= first) {
        dprintf(STDERR_FILENO, USAGE);
        return 1;
    }
    for (int i = first; i < argc; i++) {
        if ((parents ? make_parents(argv[i]) : mkdir(argv[i], 0)) < 0) {
            dprintf(STDERR_FILENO, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
