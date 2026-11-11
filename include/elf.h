#pragma once

#include <vfs.h>
#include <stdint.h>

struct elf_image {
    uint64_t entry;
    uint64_t end;
};

// Loads a static x86-64 ELF as user pages. Returns 0 or a negative errno.
int elf_load(const struct vfs_node *file, struct elf_image *image);
