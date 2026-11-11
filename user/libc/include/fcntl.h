#pragma once

#include <sys/types.h>
#include <syscall_numbers.h>

#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100

// There are no permissions, so a mode passed with O_CREAT is ignored.
int open(const char *path, int flags, ...);
int creat(const char *path, unsigned mode);
int fcntl(int fd, int command, ...);
