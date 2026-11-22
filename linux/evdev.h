#pragma once

#include <mouse_event.h>
#include <poll.h>
#include <stdint.h>

// Every keyboard, mouse and touchpad Linux has in /dev/input, taken from everything
// else while wm runs. Keys come as AximOS's scancodes.

struct evdev_input {
    void (*mouse)(const struct mouse_event *e);
    void (*key)(uint16_t code, bool pressed);
};

// Returns how many it could open, which needs root or the input group.
int evdev_open(void);
int evdev_poll_fds(struct pollfd *fds, int max);
void evdev_read(const struct pollfd *fds, const struct evdev_input *input);
void evdev_set_repeat(int delay, int rate); // in milliseconds, and times a second, or 0 for none
