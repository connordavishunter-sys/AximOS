#pragma once

#include <stddef.h>
#include <tty_types.h>

// The console as a terminal: what stands between the keyboard and read(). By default
// it echoes and hands over whole lines, and termios settings can turn that off.

int64_t tty_read(void *buf, size_t len, bool nonblocking);

// Whether a read would return at once.
bool tty_readable(void);

// Bytes that a read could return now.
size_t tty_pending(void);

void tty_get_settings(struct termios *out);
void tty_set_settings(const struct termios *settings, bool discard_input);

// Whether Ctrl+C should raise a signal. Safe to call from an interrupt.
bool tty_signals_enabled(void);

// Shows that Ctrl+C was pressed, as it's not going to reach anyone as a key.
void tty_echo_interrupt(void);
