#pragma once

#include <elf_types.h>

// Sets up the first thread from the program's headers, before anything else runs.
void __libc_set_up(const struct elf64_program_header *headers, int count) __attribute__((visibility("hidden")));

// Loads and links what the program needs, given where libc.so and the program's
// headers are. Returns false with dlerror() saying why.
bool __dl_start(char *libc_base, const struct elf64_dynamic *libc_dynamic,
                const struct elf64_program_header *headers, int count) __attribute__((visibility("hidden")));
