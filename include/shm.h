#pragma once

#include <stdint.h>

// A named block of memory that processes can map. Anything that fails returns a
// negative errno.
struct shm;

// flags are the O_ values. The object starts empty, until shm_set_size.
int shm_open(const char *name, int flags, struct shm **out);

struct shm *shm_ref(struct shm *shm);
void shm_unref(struct shm *shm);

// Removes the name. The memory stays until nothing has it open or mapped.
int shm_unlink(const char *name);

int shm_set_size(struct shm *shm, uint64_t size);
uint64_t shm_size(const struct shm *shm);

// The physical page holding a part of the object.
uint64_t shm_page(const struct shm *shm, uint64_t index);
