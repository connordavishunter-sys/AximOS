#pragma once

#include <stddef.h>

void keyboard_init(void);

// What readers sleep on until a key arrives.
void *keyboard_channel(void);

// Handles IRQ 1 and IRQ 12, which share a controller. Keys become bytes, with the
// arrow keys and other special keys as the ANSI escape sequences a terminal would
// send. Mouse data is passed on to the mouse driver.
void keyboard_handle_irq(void);

// Copies out whatever is waiting, without blocking. Returns the bytes copied.
size_t keyboard_read(char *buf, size_t len);
