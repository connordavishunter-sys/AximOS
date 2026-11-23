#include <aximos.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define FOLLOW_WAIT_NS 200000000L

static const char usage[] = "usage: tail [-f] [-n lines | -n +line | -c bytes] [file...]\n";

static long limit = 10;
static bool by_bytes, from_start;

// Reads everything, as what's wanted can't be known before the end.
static char *read_all(int fd, size_t *size) {
    char *data = NULL;
    size_t used = 0, room = 0;

    for (;;) {
        if (used == room) {
            room = room ? room * 2 : BUFFER_SIZE;
            data = realloc(data, room);
            if (!data) {
                dprintf(STDERR_FILENO, "tail: out of memory\n");
                exit(1);
            }
        }

        ssize_t got = read(fd, data + used, room - used);

        if (got <= 0)
            break;
        used += got;
    }
    *size = used;
    return data;
}

// Where the wanted part starts.
static size_t start_of_tail(const char *data, size_t size) {
    size_t limit_size = limit < 0 ? 0 : limit;

    if (by_bytes && from_start)
        return limit_size > 0 && limit_size - 1 < size ? limit_size - 1 : limit_size ? size : 0;
    if (by_bytes)
        return limit_size < size ? size - limit_size : 0;

    if (from_start) {
        size_t at = 0;

        for (long line = 1; line < limit && at < size; line++) {
            const char *newline = memchr(data + at, '\n', size - at);

            at = newline ? (size_t)(newline - data) + 1 : size;
        }
        return at;
    }

    // A line end at the very end doesn't start another line.
    size_t at = size;
    long found = 0;

    while (at > 0) {
        if (data[at - 1] == '\n' && at != size && ++found == limit)
            return at;
        at--;
    }
    return limit == 0 ? size : 0;
}

static void print_tail(int fd) {
    size_t size;
    char *data = read_all(fd, &size);
    size_t start = limit == 0 && !from_start ? size : start_of_tail(data, size);

    if (start < size) {
        fflush(stdout);
        write(STDOUT_FILENO, data + start, size - start);
    }
    free(data);
}

static void follow(int fd) {
    static char buffer[BUFFER_SIZE];

    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));

        if (got > 0) {
            write(STDOUT_FILENO, buffer, got);
        } else {
            struct timespec wait = {0, FOLLOW_WAIT_NS};

            nanosleep(&wait, NULL);
        }
    }
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: tail [-f] [-n lines | -n +line | -c bytes] [file...]\n"
                            "Prints the end of files, by default 10 lines.\n"
                            "  -n  this many lines, or from this line on if it starts with +\n"
                            "  -c  this many bytes\n"
                            "  -f  keep printing what's added until Ctrl+C\n");

    bool following = false;

    optind = 1;
    if (argc > 1 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
        limit = atol(argv[1] + 1);
        optind = 2;
    }

    int option;

    while ((option = getopt(argc, argv, "fn:c:")) != -1) {
        if (option == 'f') {
            following = true;
        } else if (option == 'n' || option == 'c') {
            from_start = optarg[0] == '+';
            limit = atol(optarg + (optarg[0] == '+' || optarg[0] == '-'));
            by_bytes = option == 'c';
        } else {
            dprintf(STDERR_FILENO, "%s", usage);
            return 1;
        }
    }

    int status = 0;
    int files = argc - optind;

    if (files == 0) {
        print_tail(STDIN_FILENO);
        return 0;
    }
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0) {
            dprintf(STDERR_FILENO, "tail: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (files > 1) {
            printf("%s==> %s <==\n", i > optind ? "\n" : "", argv[i]);
            fflush(stdout);
        }
        print_tail(fd);
        if (following && i == argc - 1)
            follow(fd);
        close(fd);
    }
    return status;
}
