#include <aximos.h>
#include <stdio.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: lsmod\n"
                            "Lists the kernel modules that are loaded, and what uses each.\n");

    FILE *f = fopen("/proc/modules", "r");
    char name[64], users[256];
    unsigned long size;
    int count;

    if (!f) {
        perror("lsmod: /proc/modules");
        return 1;
    }
    printf("%-24s %10s  %s\n", "Module", "Size", "Used by");
    while (fscanf(f, "%63s %lu %d %255s %*[^\n]", name, &size, &count, users) == 4)
        printf("%-24s %10lu  %d %s\n", name, size, count, users[0] == '-' ? "" : users);
    fclose(f);
    return 0;
}
