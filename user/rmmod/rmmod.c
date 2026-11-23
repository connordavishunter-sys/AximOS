#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: rmmod module...\n"
                            "Unloads kernel modules that nothing's using.\n");

    int failed = 0;

    for (int i = 1; i < argc; i++) {
        if (syscall(SYS_delete_module, argv[i], O_NONBLOCK) < 0) {
            fprintf(stderr, "rmmod: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
        }
    }
    return failed;
}
