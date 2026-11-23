#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define COMPRESSED 4 // MODULE_INIT_COMPRESSED_FILE: the kernel unpacks it

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: insmod file [option=value]...\n"
                            "Loads a kernel module from a file, compressed or not, without what it needs.\n"
                            "modprobe loads those too.\n");

    char options[1024] = "";

    if (argc < 2) {
        fprintf(stderr, "usage: insmod file [option=value]...\n");
        return 1;
    }
    for (int i = 2; i < argc; i++)
        snprintf(options + strlen(options), sizeof(options) - strlen(options), "%s%s", i > 2 ? " " : "", argv[i]);

    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    bool compressed = strstr(argv[1], ".ko.") != NULL;

    if (fd < 0 || syscall(SYS_finit_module, fd, options, compressed ? COMPRESSED : 0) < 0) {
        fprintf(stderr, "insmod: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
