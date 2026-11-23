#include <aximos.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_SECOND 1000000000L

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: sleep seconds...\n"
                            "Waits for that long, which can have a fraction, or for the total of several.\n");

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: sleep seconds...\n");
        return 1;
    }

    double total = 0;

    for (int i = 1; i < argc; i++) {
        char *end;
        double seconds = strtod(argv[i], &end);

        if (end == argv[i] || *end || seconds < 0) {
            dprintf(STDERR_FILENO, "sleep: %s: not a number of seconds\n", argv[i]);
            return 1;
        }
        total += seconds;
    }

    struct timespec left = {(time_t)total, (long)((total - (time_t)total) * NS_PER_SECOND)};

    while (nanosleep(&left, &left) < 0 && errno == EINTR)
        ;
    return 0;
}
