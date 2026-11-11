#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define COLUMN_WIDTH 10

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: free\n"
                            "Shows how memory is being used.\n");

    struct meminfo info;

    if (meminfo(&info) < 0) {
        dprintf(STDERR_FILENO, "free: %s\n", strerror(errno));
        return 1;
    }

    printf("%-5s %10s %10s %10s %10s %10s\n", "", "total", "used", "free", "kernel", "cached");
    printf("%-5s ", "Mem:");
    print_size(info.total, COLUMN_WIDTH);
    printf(" ");
    print_size(info.total - info.free, COLUMN_WIDTH);
    printf(" ");
    print_size(info.free, COLUMN_WIDTH);
    printf(" ");
    print_size(info.kernel, COLUMN_WIDTH);
    printf(" ");
    print_size(info.cached, COLUMN_WIDTH);
    printf("\n");
    return 0;
}
