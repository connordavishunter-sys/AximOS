#include <errno.h>
#include <fcntl.h>
#include <mouse_event.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Prints what the mouse does until Ctrl+C.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: mousetest\n"
                            "Prints what the mouse does until Ctrl+C.\n");

    int fd = open("/dev/mouse", O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "mousetest: /dev/mouse: %s\n", strerror(errno));
        return 1;
    }

    int x = 0, y = 0, wheel = 0;
    struct mouse_event events[16];
    ssize_t bytes;

    while ((bytes = read(fd, events, sizeof(events))) > 0) {
        for (size_t i = 0; i < bytes / sizeof(events[0]); i++) {
            x = events[i].absolute ? events[i].x : x + events[i].dx;
            y = events[i].absolute ? events[i].y : y + events[i].dy;
            wheel += events[i].wheel;
            printf("%sx %d y %d wheel %d buttons %c%c%c\n", events[i].absolute ? "at " : "", x, y, wheel,
                   events[i].buttons & MOUSE_BUTTON_LEFT ? 'L' : '-',
                   events[i].buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-',
                   events[i].buttons & MOUSE_BUTTON_RIGHT ? 'R' : '-');
        }
    }
    return 0;
}
