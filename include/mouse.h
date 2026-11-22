#pragma once

#include <stddef.h>
#include <stdint.h>

// Finds and starts the PS/2 mouse. Call after keyboard_init, since they share a
// controller. Returns false if there isn't one.
bool mouse_init(void);

// Takes a byte the controller says came from the mouse.
void mouse_handle_byte(uint8_t byte);

// Movement from any mouse: right and down are positive, as is the wheel turned
// towards you, and buttons are the MOUSE_BUTTON_ held.
void mouse_add_motion(int dx, int dy, int wheel, uint32_t buttons);

// Copies out as many whole events as fit, blocking until there's at least one
// unless told not to.
// Returns the bytes copied, or a negative errno.
int64_t mouse_read(void *buf, size_t len, bool nonblocking);

// Whether a read would return at once.
bool mouse_readable(void);
