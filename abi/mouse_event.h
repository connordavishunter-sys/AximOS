#pragma once

#include <stdint.h>

// Shared by the kernel and libc. Reading /dev/mouse returns whole events of this
// size, blocking until there's at least one.

#define MOUSE_BUTTON_LEFT 0x1
#define MOUSE_BUTTON_RIGHT 0x2
#define MOUSE_BUTTON_MIDDLE 0x4

struct mouse_event {
    int32_t dx;       // positive is right
    int32_t dy;       // positive is down the screen
    int32_t wheel;    // positive is towards the user
    uint32_t buttons; // which are held down after this event
};
