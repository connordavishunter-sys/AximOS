#pragma once

#include <mouse_event.h>
#include <poll.h>
#include <stdint.h>

// Every keyboard, mouse, touchpad, tablet and touchscreen Linux has in /dev/input.
// Keys come as AximOS's scancodes.

struct evdev_input {
    void (*mouse)(const struct mouse_event *e);
    void (*key)(uint16_t code, bool pressed);
};

// Opens each, through open_device, which returns its file and an id for
// close_device, as libseat does. Returns how many it could.
int evdev_open(int (*open_device)(const char *path, int *id), void (*close_device)(int id));

// Closes them all, as they no longer work once the seat's been taken away.
void evdev_close(void);

int evdev_poll_fds(struct pollfd *fds, int max);
void evdev_read(const struct pollfd *fds, const struct evdev_input *input);
void evdev_set_repeat(int delay, int rate); // in milliseconds, and times a second, or 0 for none
