#include <aximos.h>
#include <stdio.h>
#include <sys/swap.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: swapon file\n"
                            "Has Linux swap to a file or partition made for it, as the installer's\n"
                            "/swapfile is. Only root can.\n");
    if (argc != 2) {
        fprintf(stderr, "usage: swapon file\n");
        return 1;
    }
    if (swapon(argv[1], 0) < 0) {
        perror(argv[1]);
        return 1;
    }
    return 0;
}
