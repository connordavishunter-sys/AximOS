#pragma once

#include <stddef.h>
#include <stdint.h>

void keyboard_init(void);

// What readers sleep on until a key arrives.
void *keyboard_channel(void);

// Handles IRQ 1 and IRQ 12, which share a controller. Keys become bytes, with the
// arrow keys and other special keys as the ANSI escape sequences a terminal would
// send. Mouse data is passed on to the mouse driver.
void keyboard_handle_irq(void);

// Copies out whatever is waiting, without blocking. Returns the bytes copied.
size_t keyboard_read(char *buf, size_t len);

// /dev/keyboard, which only one file can have open. While it's open, keys are
// key_events for it rather than bytes for the console. These return a negative errno.
int keyboard_open_events(void);
void keyboard_close_events(void);
bool keyboard_events_readable(void);
int64_t keyboard_read_events(void *buf, size_t len, bool nonblocking);
