#pragma once

#include <stddef.h>
#include <stdint.h>
#include <tty_types.h>

// A terminal: what stands between typing and read(). By default it echoes and
// hands over whole lines, and termios settings can turn that off. The console is
// one, with the keyboard and screen, and each pseudo-terminal is another, with a
// master that a program such as a terminal window reads and writes.
struct tty;

struct tty *tty_console(void);

// The slave side, which programs read and write as their terminal.
int64_t tty_read(struct tty *tty, void *buf, size_t len, bool nonblocking);
int64_t tty_write(struct tty *tty, const void *buf, size_t len, bool nonblocking);

// Whether a read or write would return at once.
bool tty_readable(struct tty *tty);
bool tty_writable(const struct tty *tty);

// Bytes that a read could return now.
size_t tty_pending(struct tty *tty);

void tty_get_settings(const struct tty *tty, struct termios *out);
void tty_set_settings(struct tty *tty, const struct termios *settings, bool discard_input);

// The process group that gets Ctrl+C and other signals from it.
int tty_foreground(const struct tty *tty);
void tty_set_foreground(struct tty *tty, int pgid);

// The console's size is the screen's, and a pseudo-terminal's is whatever its
// master says, which tells the foreground group with SIGWINCH.
void tty_get_size(const struct tty *tty, struct winsize *out);
int tty_set_size(struct tty *tty, const struct winsize *size);

// Whether Ctrl+C should raise a signal. Safe to call from an interrupt.
bool tty_signals_enabled(const struct tty *tty);

// Shows that Ctrl+C was pressed, as it's not going to reach anyone as a key.
void tty_echo_interrupt(struct tty *tty);

// A new pseudo-terminal, held by its master, or NULL when memory runs out. The
// master, each slave file and each process it's the terminal of hold references,
// and it's freed when the last goes. These do nothing to the console.
struct tty *tty_open_pty(void);
struct tty *tty_ref(struct tty *tty);
void tty_unref(struct tty *tty);

// Closing the master hangs up: slaves read the end of the file, and writing to them fails.
void tty_close_master(struct tty *tty);
struct tty *tty_open_slave(struct tty *tty);
void tty_close_slave(struct tty *tty);

// The master reads what programs wrote, the end of the file once no slave is
// open, and writes what's typed.
int64_t tty_master_read(struct tty *tty, void *buf, size_t len, bool nonblocking);
int64_t tty_master_write(struct tty *tty, const void *buf, size_t len);
int tty_master_poll(const struct tty *tty);
