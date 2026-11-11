#pragma once

#include <syscall_numbers.h>

// Takes RB_AUTOBOOT to restart or RB_POWER_OFF to turn off. Only returns on failure.
int reboot(int command);
