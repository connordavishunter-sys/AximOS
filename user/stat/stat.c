#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *kind_of(mode_t mode) {
    switch (mode & S_IFMT) {
    case S_IFDIR: return "directory";
    case S_IFCHR: return "device";
    case S_IFIFO: return "pipe";
    case S_IFSOCK: return "socket";
    }
    return "file";
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: stat path...\n"
                            "Shows what's known about files and directories.\n");

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: stat path...\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        struct stat st;

        if (stat(argv[i], &st) < 0) {
            dprintf(STDERR_FILENO, "stat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        struct tm t;
        time_t when = st.st_mtime;
        char date[32];

        localtime_r(&when, &t);
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &t);

        printf("  Name: %s\n  Type: %s\n  Size: %lu bytes", argv[i], kind_of(st.st_mode), (unsigned long)st.st_size);
        if (st.st_size >= 1024) {
            printf(" (");
            print_size(st.st_size, 0);
            printf(")");
        }
        printf("\nBlocks: %lu of 512 bytes\nDevice: %lu\n  Node: %lu\n  Date: %s\n", (unsigned long)st.st_blocks,
               (unsigned long)st.st_dev, (unsigned long)st.st_ino, st.st_mtime ? date : "unknown");
    }
    return status;
}
