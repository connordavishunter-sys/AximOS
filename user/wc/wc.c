#include <aximos.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char buffer[65536];
static bool show_lines, show_words, show_bytes;

struct counts {
    unsigned long lines, words, bytes;
};

static bool count(int fd, struct counts *out) {
    bool in_word = false;
    ssize_t got;

    *out = (struct counts){0};
    while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
        out->bytes += got;
        for (ssize_t i = 0; i < got; i++) {
            unsigned char c = buffer[i];
            bool space = isspace(c);

            out->lines += c == '\n';
            out->words += !space && !in_word;
            in_word = !space;
        }
    }
    return got == 0;
}

// Counts are separated by one space, with none left trailing.
static void print(const struct counts *c, const char *name) {
    const unsigned long values[] = {c->lines, c->words, c->bytes};
    const bool shown[] = {show_lines, show_words, show_bytes};
    const char *gap = "";

    for (int i = 0; i < 3; i++) {
        if (shown[i]) {
            printf("%s%7lu", gap, values[i]);
            gap = " ";
        }
    }
    printf(name ? " %s\n" : "\n", name);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: wc [-lwc] [file...]\n"
                            "Counts lines, words and bytes, from standard input if no files are given.\n"
                            "  -l  only lines\n"
                            "  -w  only words\n"
                            "  -c  only bytes\n");

    int option;

    while ((option = getopt(argc, argv, "lwc")) != -1) {
        if (option == 'l') {
            show_lines = true;
        } else if (option == 'w') {
            show_words = true;
        } else if (option == 'c') {
            show_bytes = true;
        } else {
            dprintf(STDERR_FILENO, "usage: wc [-lwc] [file...]\n");
            return 1;
        }
    }
    if (!show_lines && !show_words && !show_bytes)
        show_lines = show_words = show_bytes = true;

    struct counts c, total = {0};
    int status = 0;

    if (optind == argc) {
        count(STDIN_FILENO, &c);
        print(&c, NULL);
        return 0;
    }
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0 || !count(fd, &c)) {
            dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            if (fd >= 0)
                close(fd);
            continue;
        }
        close(fd);
        print(&c, argv[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }
    if (argc - optind > 1)
        print(&total, "total");
    return status;
}
