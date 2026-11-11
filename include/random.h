#pragma once

#include <stddef.h>
#include <stdint.h>

// A ChaCha20 generator, keyed from a pool that hashes in the CPU's random number
// instruction if it has one, and the timing of every interrupt.
void random_init(void);

// Adds to the pool what can't be predicted about an interrupt. Each counts as one bit.
void random_add_interrupt(uint64_t vector);

// Adds anything at all to the pool, counting it as no bits at all.
void random_add_data(const void *data, size_t len);

// Whether enough has gone into the pool for the output to be unguessable.
bool random_ready(void);

// Fills out from the generator, ready or not.
void random_bytes(void *out, size_t len);

// Waits for random_ready. Returns a negative errno if it can't.
int random_wait_ready(bool nonblocking);
