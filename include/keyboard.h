#pragma once

#include <stddef.h>
#include <stdint.h>

void keyboard_init(void);

// Takes up the modifiers the BIOS saw held, once memory can be mapped.
void keyboard_read_firmware_modifiers(void);

// The KEY_MOD_ held now.
uint8_t keyboard_modifiers(void);

// What readers sleep on until a key arrives.
void *keyboard_channel(void);

// Handles IRQ 1 and IRQ 12, which share a controller. Keys become bytes, with the
// arrow keys and other special keys as the ANSI escape sequences a terminal would
// send. Mouse data is passed on to the mouse driver.
// Also called once interrupts are routed, for anything that came before.
void keyboard_handle_irq(void);

// Called on every tick, to repeat the key held down.
void keyboard_tick(void);

// How long, in milliseconds, before a held key repeats, and how many times a
// second then. A negative one is left as it is.
void keyboard_set_repeat(int delay_ms, int per_second);

// Takes a byte the controller says came from the keyboard.
void keyboard_handle_byte(uint8_t byte);

// Copies out whatever is waiting, without blocking. Returns the bytes copied.
size_t keyboard_read(char *buf, size_t len);

// /dev/keyboard, which only one file can have open. While it's open, keys are
// key_events for it rather than bytes for the console. These return a negative errno.
int keyboard_open_events(void);
void keyboard_close_events(void);
bool keyboard_events_readable(void);
int64_t keyboard_read_events(void *buf, size_t len, bool nonblocking);
