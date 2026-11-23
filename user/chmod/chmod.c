#include <aximos.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// A mode as a number, like 755, or as changes, like u+x or go-w, separated by commas.
static bool apply(const char *spec, mode_t *mode) {
    char *end;
    long number = strtol(spec, &end, 8);

    if (!*end && end != spec) {
        *mode = number & 07777;
        return true;
    }
    for (const char *p = spec; *p;) {
        mode_t who = 0, bits = 0;

        for (; strchr("ugoa", *p) && *p; p++)
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        who = who ? who : 07777 & ~umask(umask(0));
        if (!strchr("+-=", *p) || !*p)
            return false;

        char op = *p++;

        for (; *p && *p != ','; p++) {
            const char *at = strchr("rwxst", *p);

            if (!at)
                return false;
            bits |= (mode_t[]){0444, 0222, 0111, 06000, 01000}[at - "rwxst"];
        }
        *mode = op == '+' ? *mode | (bits & who) : op == '-' ? *mode & ~(bits & who) : (*mode & ~who) | (bits & who);
        p += *p == ',';
    }
    return true;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: chmod mode file...\n"
                            "Changes who may read, write and run files. The mode is a number, like 755,\n"
                            "or changes, like u+x or go-w: u the owner, g the group, o everyone else,\n"
                            "a all of them, then + - or =, then r w x, s for setuid and t for sticky.\n");

    int failed = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: chmod mode file...\n");
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        struct stat st;
        mode_t mode;

        errno = 0;
        if (stat(argv[i], &st) < 0 || (mode = st.st_mode & 07777, !apply(argv[1], &mode)) || chmod(argv[i], mode) < 0) {
            fprintf(stderr, "chmod: %s: %s\n", argv[i], errno ? strerror(errno) : "not a mode");
            failed = 1;
        }
    }
    return failed;
}
