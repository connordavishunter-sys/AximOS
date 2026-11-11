#pragma once

#include <stddef.h>
#include <sys/types.h>

#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
#define GRND_INSECURE 4

// Fills buf with random bytes, waiting first if the system has only just started.
ssize_t getrandom(void *buf, size_t len, unsigned flags);
int getentropy(void *buf, size_t len);
