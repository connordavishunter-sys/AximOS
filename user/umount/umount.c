#include <aximos.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: umount [-l] folder...\n"
                            "Unmounts the filesystems on the folders.\n"
                            "  -l  take it away now, and finish once nothing's using it\n");

    int flags = 0, failed = 0, option;

    while ((option = getopt(argc, argv, "l")) != -1) {
        if (option != 'l')
            return 1;
        flags = MNT_DETACH;
    }
    for (int i = optind; i < argc; i++) {
        if (umount2(argv[i], flags) < 0) {
            fprintf(stderr, "umount: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
        }
    }
    return failed;
}
