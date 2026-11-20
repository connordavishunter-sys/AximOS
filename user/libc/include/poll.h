#pragma once

#include <fs_types.h>

// Urgent and out-of-band data never come, and normal data is all there is.
#define POLLPRI 0x002
#define POLLRDBAND 0x080
#define POLLWRBAND 0x200
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT

typedef unsigned long nfds_t;

// Waits up to timeout milliseconds, or forever if negative, returning how many
// have events.
int poll(struct pollfd *fds, nfds_t count, int timeout);
