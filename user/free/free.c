#include <aximos.h>
#include <stdio.h>

#define COLUMN_WIDTH 10

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: free\n"
                            "Shows how memory is being used. Available is what programs could still\n"
                            "have, counting what's only caching files.\n");

    struct memory m;

    if (!memory_read(&m)) {
        perror("free: /proc/meminfo");
        return 1;
    }
    printf("%-5s %10s %10s %10s %10s %10s\n", "", "total", "used", "free", "cached", "available");
    printf("%-5s ", "Mem:");
    uint64_t columns[] = {m.total, m.total - m.available, m.free, m.cached, m.available};

    for (int i = 0; i < 5; i++) {
        print_size(columns[i], COLUMN_WIDTH);
        printf(i < 4 ? " " : "\n");
    }
    return 0;
}
