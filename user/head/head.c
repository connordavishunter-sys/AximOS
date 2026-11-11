#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char usage[] = "usage: head [-n lines | -c bytes] [file...]\n";

static long limit = 10;
static bool by_bytes;

static void copy_start(FILE *in) {
    long left = limit;
    int c;

    while (left > 0 && (c = getc(in)) != EOF) {
        putchar(c);
        if (by_bytes || c == '\n')
            left--;
    }
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: head [-n lines | -c bytes] [file...]\n"
                            "Prints the start of files, by default 10 lines.\n"
                            "  -n  this many lines, which can also be written -5\n"
                            "  -c  this many bytes\n");

    int first = 1;

    if (argc > 1 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
        limit = atol(argv[1] + 1);
        first = 2;
    }
    optind = first;

    int option;

    while ((option = getopt(argc, argv, "n:c:")) != -1) {
        if (option == '?') {
            dprintf(STDERR_FILENO, "%s", usage);
            return 1;
        }
        limit = atol(optarg);
        by_bytes = option == 'c';
    }

    int status = 0;
    int files = argc - optind;

    if (files == 0)
        copy_start(stdin);
    for (int i = optind; i < argc; i++) {
        FILE *in = fopen(argv[i], "r");

        if (!in) {
            dprintf(STDERR_FILENO, "head: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (files > 1)
            printf("%s==> %s <==\n", i > optind ? "\n" : "", argv[i]);
        copy_start(in);
        fclose(in);
    }
    return status;
}
