#pragma once

#include <stddef.h>

// Returns zeroed memory aligned to 16 bytes, or NULL. It never crosses a page,
// and the largest size is KMALLOC_MAX.
void *kmalloc(size_t size);
void kfree(void *ptr);

#define KMALLOC_MAX 4064
