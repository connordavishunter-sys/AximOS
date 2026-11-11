#pragma once

#include <stdint.h>

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

// zone is ignored, since UTC is the only time zone.
int gettimeofday(struct timeval *out, void *zone);

// The clock can't be set yet, so this fails with EPERM.
int settimeofday(const struct timeval *value, const void *zone);

// Files only get a time when they're written, so these just check the file exists.
int utimes(const char *path, const struct timeval times[2]);
int lutimes(const char *path, const struct timeval times[2]);
int futimes(int fd, const struct timeval times[2]);
