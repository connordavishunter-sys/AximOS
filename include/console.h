#pragma once

#include <stddef.h>
#include <stdint.h>

// The text screen programs talk to. Understands the ANSI escape sequences for
// cursor movement and erasing. Output is mirrored to serial for logging.
//
// Draws on a 32-bit screen. Setting one starts again with a blank page.
void console_set_screen(uint8_t *base, uint64_t width, uint64_t height, uint64_t pitch);

// Moves the same screen to another mapping of it, keeping everything else.
void console_set_base(uint8_t *base);

// While hidden, text is still kept, and shows up when the console is next shown.
void console_hide(void);
void console_show(void);

void console_write(const char *buf, size_t len);
void console_size(int *rows, int *cols);
