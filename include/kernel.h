#pragma once

[[noreturn]] void halt(void);
#include <stdint.h>

// Stops the machine with the message and where it was on the screen, rip and rbp
// being where the kernel went wrong, or die's own caller.
[[noreturn]] void die(const char *message);
[[noreturn]] void die_at(const char *message, uint64_t rip, uint64_t rbp);
