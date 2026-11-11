#pragma once

#include <time.h>

struct utimbuf {
    time_t actime;
    time_t modtime;
};

// Files only get a time when they're written, so this just checks the path exists.
int utime(const char *path, const struct utimbuf *times);
