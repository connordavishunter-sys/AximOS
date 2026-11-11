#pragma once

#include <socket_types.h>
#include <sys/types.h>

ssize_t readv(int fd, const struct iovec *parts, int count);
ssize_t writev(int fd, const struct iovec *parts, int count);
