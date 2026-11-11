// libc.so is also the dynamic linker: the kernel starts a dynamically linked program
// here, with libc.so loaded but not yet relocated. Until relocate_self has run, the
// code can't touch a global variable or call anything outside this file.
#include <dlfcn.h>
#include <elf_types.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "start.h"
#include "syscall.h"

extern const struct elf64_dynamic _DYNAMIC[] __attribute__((visibility("hidden")));

uint64_t __dl_main(uint64_t *sp) __attribute__((visibility("hidden")));

// Leaves the stack as the kernel made it for the program, which starts as if it had
// been run directly.
__asm__(".text\n"
        ".global _dlstart\n"
        ".type _dlstart, @function\n"
        "_dlstart:\n"
        "    xorl %ebp, %ebp\n"
        "    movq %rsp, %r12\n"
        "    movq %rsp, %rdi\n"
        "    andq $-16, %rsp\n"
        "    call __dl_main\n"
        "    movq %r12, %rsp\n"
        "    xorl %edx, %edx\n"
        "    jmp *%rax\n");

// The compiler mustn't turn these loops into calls to memset or memcpy.
#define EARLY __attribute__((optimize("no-tree-loop-distribute-patterns")))

// Points libc at itself. Where the program has its own copy of something, the
// second pass in dl.c points libc at that instead.
EARLY static void apply(char *base, const struct elf64_symbol *symbols, const struct elf64_rela *r, size_t size) {
    for (const struct elf64_rela *end = (const void *)((const char *)r + size); r < end; r++) {
        uint64_t *where = (uint64_t *)(base + r->r_offset);
        uint32_t type = r->r_info;
        const struct elf64_symbol *symbol = &symbols[r->r_info >> 32];

        if (type == R_X86_64_RELATIVE)
            *where = (uint64_t)base + r->r_addend;
        else if (symbol->st_shndx != SHN_UNDEF)
            *where = (uint64_t)base + symbol->st_value + (type == R_X86_64_64 ? r->r_addend : 0);
    }
}

EARLY static void relocate_self(char *base) {
    const struct elf64_symbol *symbols = NULL;
    const struct elf64_rela *rela = NULL, *plt = NULL;
    size_t rela_size = 0, plt_size = 0;

    for (const struct elf64_dynamic *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB)
            symbols = (const void *)(base + d->d_val);
        else if (d->d_tag == DT_RELA)
            rela = (const void *)(base + d->d_val);
        else if (d->d_tag == DT_RELASZ)
            rela_size = d->d_val;
        else if (d->d_tag == DT_JMPREL)
            plt = (const void *)(base + d->d_val);
        else if (d->d_tag == DT_PLTRELSZ)
            plt_size = d->d_val;
    }
    if (rela)
        apply(base, symbols, rela, rela_size);
    if (plt)
        apply(base, symbols, plt, plt_size);
}

// Returns where the program starts.
EARLY uint64_t __dl_main(uint64_t *sp) {
    char **envp = (char **)sp + 1 + sp[0] + 1;
    char *base = NULL;
    const struct elf64_program_header *headers = NULL;
    int count = 0;
    uint64_t entry = 0;

    while (*envp)
        envp++;
    for (uint64_t *aux = (uint64_t *)(envp + 1); aux[0] != AT_NULL; aux += 2) {
        if (aux[0] == AT_BASE)
            base = (char *)aux[1];
        else if (aux[0] == AT_PHDR)
            headers = (const void *)aux[1];
        else if (aux[0] == AT_PHNUM)
            count = aux[1];
        else if (aux[0] == AT_ENTRY)
            entry = aux[1];
    }
    if (!base || !headers) // run as a program itself, which it isn't
        syscall3(SYS_EXIT, 127, 0, 0);

    relocate_self(base);
    __libc_set_up(headers, count);
    if (!__dl_start(base, _DYNAMIC, headers, count)) {
        dprintf(STDERR_FILENO, "dynamic linker: %s\n", dlerror());
        _exit(127);
    }
    return entry;
}
