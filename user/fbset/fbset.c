#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <unistd.h>

// Shows the resolution, or changes it with: fbset WIDTH HEIGHT
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: fbset [width height]\n"
                            "Shows the screen resolution, or changes it.\n");

    if (argc != 1 && argc != 3) {
        dprintf(STDERR_FILENO, "usage: fbset [width height]\n");
        return 1;
    }

    int fd = open("/dev/fb0", argc == 3 ? O_RDWR : O_RDONLY);
    struct fb_info info;

    if (fd < 0 || (argc == 3 && fb_set_mode(fd, atoi(argv[1]), atoi(argv[2])) < 0) || fb_info(fd, &info) < 0) {
        dprintf(STDERR_FILENO, "fbset: %s\n", strerror(errno));
        return 1;
    }

    close(fd);
    printf("%ux%u, ", info.width, info.height);
    print_size(info.memory_size, 0);
    printf(" of video memory, resolution %s\n", info.can_set_mode ? "can change" : "is fixed");
    return 0;
}
