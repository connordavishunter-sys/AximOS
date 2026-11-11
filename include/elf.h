#pragma once

#include <vfs.h>
#include <stdint.h>

struct elf_image {
    uint64_t entry;            // where to start: the dynamic linker's entry if there is one
    uint64_t end;              // the end of the program, where its heap starts
    uint64_t program_entry;    // the program's own entry point
    uint64_t phdr;             // where the program's headers are, for the dynamic linker
    uint16_t phnum;
    uint64_t interpreter_base; // where the dynamic linker went, or 0 without one
};

// Loads an x86-64 program as user pages, and the dynamic linker its PT_INTERP names.
// Returns 0 or a negative errno.
int elf_load(const struct vfs_node *file, struct elf_image *image);

// Drops what's cached of a file, which has changed or gone.
void elf_forget(const struct vfs_node *file);

// Gives back the cache's pages, returning false if there were none.
bool elf_reclaim(void);
