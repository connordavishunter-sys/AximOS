#pragma once

#include <fs_types.h>

typedef unsigned long nfds_t;

// Waits up to timeout milliseconds, or forever if negative, returning how many
// have events.
int poll(struct pollfd *fds, nfds_t count, int timeout);
