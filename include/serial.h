#pragma once

#include <stddef.h>
#include <stdint.h>

void serial_write(const char *buf, size_t len);

// Supports %s, %c, %u, %x, and %lu, %lx for 64-bit values.
__attribute__((format(printf, 1, 2)))
void kprintf(const char *fmt, ...);

// What kprintf wrote, kept from boot as far as the log's size goes, for /proc/log.
// Offsets count from the oldest byte still kept.
size_t log_size(void);
size_t log_read(uint64_t offset, void *buf, size_t len);

// Sends what kprintf writes to the screen too, starting with what's in the log
// already, as there's no other way to see it on a machine without a serial port.
void log_to_screen(bool on);

// For a kernel that's dying: shows the console, whatever had the screen, with
// the end of the log on it, and keeps writing there.
void log_show_crash(void);
