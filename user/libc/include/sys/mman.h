#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <syscall_numbers.h>

#define MAP_FAILED ((void *)-1)

// Maps len bytes, chosen by the kernel, of memory or of a shm_open object. addr
// is only a hint and is currently ignored. Memory is ready at once rather than
// on first use, so a mapping can fail with ENOMEM up front.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t len);

// Changes what's allowed on whole pages. PROT_NONE keeps them mapped but unusable.
int mprotect(void *addr, size_t len, int prot);

// Shared memory has no file behind it to write back to.
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4
int msync(void *addr, size_t len, int flags);

// Opens a named block of shared memory, sized with ftruncate. These aren't files
// on any disk, and read and write don't work on them.
int shm_open(const char *name, int flags, unsigned mode);
int shm_unlink(const char *name);
