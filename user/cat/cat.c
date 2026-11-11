#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

static int copy_to_stdout(int fd) {
    static char buffer[BUFFER_SIZE];
    ssize_t count;

    while ((count = read(fd, buffer, BUFFER_SIZE)) > 0)
        write(STDOUT_FILENO, buffer, count);
    return count < 0 ? 1 : 0;
}

static int cat_file(const char *path) {
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        dprintf(STDERR_FILENO, "cat: %s: %s\n", path, strerror(errno));
        return 1;
    }

    int status = copy_to_stdout(fd);
    close(fd);

    if (status)
        dprintf(STDERR_FILENO, "cat: %s: %s\n", path, strerror(errno));
    return status;
}

// With no files this reads standard input, which on the keyboard runs until
// Ctrl+C since it has no end of file yet.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: cat [file...]\n"
                            "Prints files, or standard input if none are given.\n");

    if (argc < 2)
        return copy_to_stdout(STDIN_FILENO);

    int status = 0;
    for (int i = 1; i < argc; i++)
        status |= cat_file(argv[i]);
    return status;
}
