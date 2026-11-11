#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int parse_number(const char *text, bool *ok) {
    bool negative = *text == '-';
    int value = 0;

    if (negative)
        text++;

    *ok = *text != '\0';
    for (; *text; text++) {
        if (*text < '0' || *text > '9')
            *ok = false;
        value = value * 10 + (*text - '0');
    }
    return negative ? -value : value;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: kill [-signal] pid...\n"
                            "Sends a signal, by number, to processes. The default is SIGTERM (15).\n");

    int signal_number = SIGTERM;
    int first = 1;

    if (argc > 1 && argv[1][0] == '-' && argc > 2) {
        bool ok;

        signal_number = parse_number(&argv[1][1], &ok);
        if (!ok) {
            dprintf(STDERR_FILENO, "kill: %s: bad signal\n", argv[1]);
            return 1;
        }
        first = 2;
    }

    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: kill [-signal] pid...\n");
        return 1;
    }

    int status = 0;
    for (int i = first; i < argc; i++) {
        bool ok;
        int pid = parse_number(argv[i], &ok);

        if (!ok || kill(pid, signal_number) < 0) {
            dprintf(STDERR_FILENO, "kill: %s: %s\n", argv[i], ok ? strerror(errno) : "bad pid");
            status = 1;
        }
    }
    return status;
}
