#pragma once

#include <stddef.h>

void serial_write(const char *buf, size_t len);

// Supports %s, %c, %u, %x, and %lu, %lx for 64-bit values.
__attribute__((format(printf, 1, 2)))
void kprintf(const char *fmt, ...);
