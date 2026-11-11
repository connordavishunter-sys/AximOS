#pragma once

#include <fs_types.h>

#include <sys/types.h>

int stat(const char *path, struct stat *out);
int fstat(int fd, struct stat *out);
int lstat(const char *path, struct stat *out); // there are no symbolic links

// There are no permissions, so these only check the file exists.
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);

// Remembers the value but nothing uses it.
mode_t umask(mode_t mask);

// There are no permissions yet, so mode is ignored.
int mkdir(const char *path, unsigned mode);

// Devices and pipes can't be made on the disk, so these fail with EPERM.
int mknod(const char *path, mode_t mode, dev_t device);
int mkfifo(const char *path, mode_t mode);

// Files only get a time when they're written, so these just check the file exists.
#define UTIME_NOW ((1l << 30) - 1)
#define UTIME_OMIT ((1l << 30) - 2)
int utimensat(int directory, const char *path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
