#pragma once

#include <string.h>
#include <sys/time.h>

// A process can have at most this many open files.
#define FD_SETSIZE 64

typedef struct {
    uint64_t bits;
} fd_set;

#define FD_ZERO(set) ((set)->bits = 0)
#define FD_SET(fd, set) ((set)->bits |= 1ull << (fd))
#define FD_CLR(fd, set) ((set)->bits &= ~(1ull << (fd)))
#define FD_ISSET(fd, set) (((set)->bits >> (fd)) & 1)

// timeout isn't updated with the time left, which POSIX allows.
int select(int count, fd_set *read_set, fd_set *write_set, fd_set *error_set, struct timeval *timeout);
