#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Creates files that don't exist yet. There's no clock to update timestamps with.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: touch file...\n"
                            "Creates files that don't exist yet.\n");

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: touch file...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);

        if (fd < 0) {
            dprintf(STDERR_FILENO, "touch: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        close(fd);
    }
    return status;
}
