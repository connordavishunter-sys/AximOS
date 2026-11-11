#pragma once

#include <stdint.h>

struct clock_time {
    int year;
    int month; // 1 to 12
    int day;   // 1 to 31
    int hour;
    int minute;
    int second;
};

#define NS_PER_SECOND 1000000000ULL

// Measures the timestamp counter. Needs the APICs set up.
void clock_init(void);

// Nanoseconds since boot, which never go backwards. The kernel lock must be held.
uint64_t clock_monotonic_ns(void);

// What clock_now said at the moment clock_monotonic_ns was zero.
int64_t clock_boot_epoch(void);

// Seconds since 1970 in UTC, read from the real-time clock.
int64_t clock_now(void);

int64_t clock_to_epoch(const struct clock_time *time);
void clock_from_epoch(int64_t epoch, struct clock_time *time);
