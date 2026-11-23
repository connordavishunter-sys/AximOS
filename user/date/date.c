#include <aximos.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: date [-u] [+format]\n"
                            "Prints the date and time in the local time zone, or in UTC with -u. format is\n"
                            "as strftime's, \"%a %b %e %H:%M:%S %Z %Y\" unless given.\n");

    const char *format = "%a %b %e %H:%M:%S %Z %Y";
    bool utc = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            utc = true;
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;
        } else {
            dprintf(STDERR_FILENO, "date: unknown argument %s\n", argv[i]);
            return 1;
        }
    }

    time_t now = time(NULL);
    struct tm t;
    char text[256];

    if (utc)
        gmtime_r(&now, &t);
    else
        localtime_r(&now, &t);
    if (!strftime(text, sizeof(text), format, &t) && *format) {
        dprintf(STDERR_FILENO, "date: format too long\n");
        return 1;
    }
    puts(text);
    return 0;
}
