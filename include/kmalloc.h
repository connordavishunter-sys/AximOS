#pragma once

#include <stddef.h>

// Returns zeroed memory aligned to 16 bytes, or NULL. Up to KMALLOC_MAX it never
// crosses a page, and it's in the direct map, so mm_virt_to_phys works on it.
void *kmalloc(size_t size);
void kfree(void *ptr);

#define KMALLOC_MAX 4064
