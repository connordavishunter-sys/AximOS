#include <aximos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <unistd.h>

#define READ_ALL 3
#define CLEAR 5
#define SIZE_BUFFER 10

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: dmesg [-c]\n"
                            "Shows what the kernel has logged.\n"
                            "  -c  clear it afterwards\n");

    int size = klogctl(SIZE_BUFFER, NULL, 0);
    char *log = size > 0 ? malloc(size) : NULL;
    int got = log ? klogctl(READ_ALL, log, size) : -1;

    if (got < 0) {
        perror("dmesg");
        return 1;
    }
    // Each line starts with its level, as <6>, which is left out.
    for (char *line = log; line < log + got;) {
        char *end = memchr(line, '\n', log + got - line);
        char *text = line[0] == '<' && strchr(line, '>') && strchr(line, '>') < (end ? end : log + got) ? strchr(line, '>') + 1 : line;

        fwrite(text, 1, (end ? end + 1 : log + got) - text, stdout);
        line = end ? end + 1 : log + got;
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'c')
        klogctl(CLEAR, NULL, 0);
    return 0;
}
