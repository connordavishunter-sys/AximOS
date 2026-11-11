#pragma once

#include <stdint.h>

// Shared by the kernel and libc.

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
