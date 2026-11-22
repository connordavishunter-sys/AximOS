#pragma once

#include <stdint.h>

// Writes where the kernel was, by function name: rip if it isn't 0, then each
// return address up the frame pointers from rbp.
void trace_print(uint64_t rip, uint64_t rbp);
