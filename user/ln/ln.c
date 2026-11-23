#include <aximos.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ln [-sf] target... name\n"
                            "Makes a link to a file. Given a folder last, the links go in it, by\n"
                            "the targets' names.\n"
                            "  -s  a symbolic link, which holds the target's path\n"
                            "  -f  replace whatever already has the name\n");

    bool symbolic = false, force = false;
    int option, failed = 0;
    struct stat st;

    while ((option = getopt(argc, argv, "sf")) != -1) {
        if (option == 's')
            symbolic = true;
        else if (option == 'f')
            force = true;
        else
            return 1;
    }
    if (argc - optind < 2) {
        fprintf(stderr, "usage: ln [-sf] target... name\n");
        return 1;
    }

    const char *last = argv[argc - 1];
    bool into = stat(last, &st) == 0 && S_ISDIR(st.st_mode);

    for (int i = optind; i < argc - 1; i++) {
        char path[PATH_MAX], copy[PATH_MAX];

        snprintf(copy, sizeof(copy), "%s", argv[i]);
        snprintf(path, sizeof(path), into ? "%s/%s" : "%s", last, basename(copy));
        if (force)
            unlink(path);
        if ((symbolic ? symlink(argv[i], path) : link(argv[i], path)) < 0) {
            fprintf(stderr, "ln: %s: %s\n", path, strerror(errno));
            failed = 1;
        }
    }
    return failed;
}
