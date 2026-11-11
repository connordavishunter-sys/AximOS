#pragma once

#include <tty_types.h>

// The console takes the TC and TIOC requests, and it and pipes take FIONREAD.
int ioctl(int fd, unsigned long request, ...);
