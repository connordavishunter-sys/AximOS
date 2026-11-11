#include <dlfcn.h>
#include <elf_types.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "lock.h"
#include "syscall.h"

#define LIBRARY_DIR "/system/lib"
#define LIBC_NAME "libc.so"
#define PAGE_SIZE 4096
#define ERROR_SIZE 512
#define MAX_NEEDED 32

struct library {
    struct library *next;
    char *path;
    int refs;
    bool global;
    char *base;           // what addresses in the file are relative to
    size_t map_size;
    char *map;
    const struct elf64_symbol *symbols;
    const char *strings;
    uint32_t symbol_count;
    const struct elf64_dynamic *dynamic;
    struct library *needed[MAX_NEEDED];
    int needed_count;
    void (*const *fini_array)(void);
    size_t fini_count;
    void (*fini)(void);
};

static struct library *libraries;
static recursive_lock dl_lock;
static char error_text[ERROR_SIZE];
static bool has_error;

// The program's own symbols, read from its file once they're first wanted.
static const struct elf64_symbol *program_symbols;
static const char *program_strings;
static size_t program_symbol_count;
static bool program_loaded;

static void set_error(const char *fmt, const char *a, const char *b) {
    snprintf(error_text, sizeof(error_text), fmt, a, b);
    has_error = true;
}

static bool read_at(int fd, void *dest, size_t len, long offset) {
    char *out = dest;

    while (len > 0) {
        ssize_t got = pread(fd, out, len, offset);

        if (got <= 0)
            return false;
        out += got;
        len -= got;
        offset += got;
    }
    return true;
}

// A failure only means symbols in the program can't be used. That only matters to a
// library that wants them, which then says which one it couldn't find.
static void load_program_symbols(void) {
    char path[PATH_MAX];

    if (program_loaded)
        return;
    program_loaded = true;

    if (syscall3(SYS_EXE_PATH, (long)path, sizeof(path), 0) < 0)
        return;

    int fd = open(path, O_RDONLY);
    struct elf64_header header;

    if (fd < 0)
        return;

    struct elf64_section_header *sections = NULL;

    if (read_at(fd, &header, sizeof(header), 0)) {
        size_t size = header.e_shnum * sizeof(*sections);

        sections = malloc(size ? size : 1);
        if (sections && !read_at(fd, sections, size, header.e_shoff)) {
            free(sections);
            sections = NULL;
        }
    }

    for (int i = 0; sections && i < header.e_shnum; i++) {
        if (sections[i].sh_type != SHT_SYMTAB || sections[i].sh_link >= header.e_shnum)
            continue;

        const struct elf64_section_header *strings = &sections[sections[i].sh_link];
        void *symbol_data = malloc(sections[i].sh_size);
        char *string_data = malloc(strings->sh_size);

        if (symbol_data && string_data && read_at(fd, symbol_data, sections[i].sh_size, sections[i].sh_offset)
            && read_at(fd, string_data, strings->sh_size, strings->sh_offset)) {
            program_symbols = symbol_data;
            program_strings = string_data;
            program_symbol_count = sections[i].sh_size / sizeof(struct elf64_symbol);
        }
        break;
    }
    free(sections);
    close(fd);
}

static bool symbol_usable(const struct elf64_symbol *symbol) {
    int binding = symbol->st_info >> 4;

    return symbol->st_shndx != SHN_UNDEF && (binding == STB_GLOBAL || binding == STB_WEAK);
}

static void *find_in_program(const char *name) {
    load_program_symbols();
    for (size_t i = 0; i < program_symbol_count; i++) {
        const struct elf64_symbol *symbol = &program_symbols[i];

        if (symbol_usable(symbol) && strcmp(program_strings + symbol->st_name, name) == 0)
            return (void *)symbol->st_value;
    }
    return NULL;
}

static void *find_in_library(const struct library *library, const char *name) {
    for (uint32_t i = 0; i < library->symbol_count; i++) {
        const struct elf64_symbol *symbol = &library->symbols[i];

        if (symbol_usable(symbol) && strcmp(library->strings + symbol->st_name, name) == 0)
            return library->base + symbol->st_value;
    }
    return NULL;
}

// Depth first through what a library needs.
static void *find_in_tree(const struct library *library, const char *name) {
    void *found = find_in_library(library, name);

    for (int i = 0; !found && i < library->needed_count; i++)
        found = find_in_tree(library->needed[i], name);
    return found;
}

// The program first, then libraries opened as global, then the one asking and what
// it depends on. The first definition wins, so everyone agrees on one malloc.
static void *resolve(const struct library *from, const char *name) {
    void *found = find_in_program(name);

    for (const struct library *l = libraries; !found && l; l = l->next) {
        if (l->global)
            found = find_in_library(l, name);
    }
    return found || !from ? found : find_in_tree(from, name);
}

static const uint64_t *find_dynamic(const struct library *library, int64_t tag) {
    for (const struct elf64_dynamic *d = library->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == tag)
            return &d->d_val;
    }
    return NULL;
}

static bool relocate(struct library *library, const struct elf64_rela *relocations, size_t size) {
    size_t count = size / sizeof(*relocations);

    for (size_t i = 0; i < count; i++) {
        const struct elf64_rela *r = &relocations[i];
        uint64_t *where = (uint64_t *)(library->base + r->r_offset);
        uint32_t type = r->r_info;
        const struct elf64_symbol *symbol = &library->symbols[r->r_info >> 32];
        char *value = NULL;

        if (type == R_X86_64_RELATIVE) {
            *where = (uint64_t)library->base + r->r_addend;
            continue;
        }
        if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
            char number[16];

            snprintf(number, sizeof(number), "%u", type);
            set_error("%s: unsupported relocation type %s", library->path, number);
            return false;
        }

        const char *name = library->strings + symbol->st_name;

        value = resolve(library, name);
        if (!value && (symbol->st_info >> 4) != STB_WEAK) {
            set_error("%s: undefined symbol %s", library->path, name);
            return false;
        }
        *where = value ? (uint64_t)value + (type == R_X86_64_64 ? r->r_addend : 0) : 0;
    }
    return true;
}

static struct library *open_library(const char *name, bool global);

static bool library_is_called(const struct library *library, const char *name) {
    const uint64_t *soname = find_dynamic(library, DT_SONAME);

    if (soname && strcmp(library->strings + *soname, name) == 0)
        return true;

    const char *slash = strrchr(library->path, '/');

    return strcmp(slash ? slash + 1 : library->path, name) == 0;
}

static void close_library(struct library *library) {
    if (--library->refs > 0)
        return;

    for (size_t i = library->fini_count; i > 0; i--)
        library->fini_array[i - 1]();
    if (library->fini)
        library->fini();

    for (struct library **link = &libraries; *link; link = &(*link)->next) {
        if (*link == library) {
            *link = library->next;
            break;
        }
    }
    for (int i = 0; i < library->needed_count; i++)
        close_library(library->needed[i]);

    munmap(library->map, library->map_size);
    free(library->path);
    free(library);
}

// Maps the file's segments where its addresses say, relative to one free region.
static bool map_segments(struct library *library, int fd, const struct elf64_header *header,
                         const struct elf64_program_header *headers) {
    uint64_t low = UINT64_MAX;
    uint64_t high = 0;

    for (int i = 0; i < header->e_phnum; i++) {
        const struct elf64_program_header *ph = &headers[i];

        if (ph->p_type == PT_TLS && ph->p_memsz) {
            set_error("%s: libraries can't use __thread variables%s", library->path, "");
            return false;
        }
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_vaddr < low)
            low = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > high)
            high = ph->p_vaddr + ph->p_memsz;
    }
    if (low == UINT64_MAX) {
        set_error("%s: nothing to load%s", library->path, "");
        return false;
    }

    low &= ~(uint64_t)(PAGE_SIZE - 1);
    library->map_size = (high - low + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    library->map = mmap(NULL, library->map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (library->map == MAP_FAILED) {
        library->map = NULL;
        set_error("%s: %s", library->path, strerror(errno));
        return false;
    }
    library->base = library->map - low;

    for (int i = 0; i < header->e_phnum; i++) {
        const struct elf64_program_header *ph = &headers[i];

        if (ph->p_type == PT_LOAD && ph->p_filesz && !read_at(fd, library->base + ph->p_vaddr, ph->p_filesz, ph->p_offset)) {
            set_error("%s: can't read a segment%s", library->path, "");
            return false;
        }
        if (ph->p_type == PT_DYNAMIC)
            library->dynamic = (const void *)(library->base + ph->p_vaddr);
    }
    return true;
}

// Only what relocation needed to write to stays writable, once it's done.
static void protect_segments(const struct library *library, const struct elf64_header *header,
                             const struct elf64_program_header *headers) {
    for (int i = 0; i < header->e_phnum; i++) {
        const struct elf64_program_header *ph = &headers[i];

        if (ph->p_type != PT_LOAD)
            continue;

        uintptr_t start = (uintptr_t)(library->base + ph->p_vaddr) & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t end = (uintptr_t)(library->base + ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
        int prot = (ph->p_flags & 4 ? PROT_READ : 0) | (ph->p_flags & 2 ? PROT_WRITE : 0) | (ph->p_flags & 1 ? PROT_EXEC : 0);

        mprotect((void *)start, end - start, prot);
    }
}

static bool find_tables(struct library *library) {
    const uint64_t *symbols = library->dynamic ? find_dynamic(library, DT_SYMTAB) : NULL;
    const uint64_t *strings = library->dynamic ? find_dynamic(library, DT_STRTAB) : NULL;
    const uint64_t *hash = library->dynamic ? find_dynamic(library, DT_HASH) : NULL;

    if (!symbols || !strings || !hash) {
        set_error("%s: needs a dynamic section with a DT_HASH table (link with --hash-style=sysv)%s", library->path, "");
        return false;
    }

    library->symbols = (const void *)(library->base + *symbols);
    library->strings = library->base + *strings;
    library->symbol_count = ((const uint32_t *)(library->base + *hash))[1]; // the chain count
    return true;
}

static bool load_needed(struct library *library) {
    // Anything it wants that the program doesn't have comes from here.
    bool has_libc = false;

    for (const struct elf64_dynamic *d = library->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED)
            continue;
        if (library->needed_count == MAX_NEEDED) {
            set_error("%s: needs too many libraries%s", library->path, "");
            return false;
        }

        const char *name = library->strings + d->d_val;

        if (strcmp(name, LIBC_NAME) == 0 || strcmp(name, "libc.so.6") == 0) {
            has_libc = true;
            name = LIBC_NAME;
        }

        struct library *needed = open_library(name, false);

        if (!needed)
            return false;
        library->needed[library->needed_count++] = needed;
    }

    if (!has_libc && strstr(library->path, "/" LIBC_NAME) == NULL) {
        struct library *libc = open_library(LIBC_NAME, false);

        if (libc && library->needed_count < MAX_NEEDED)
            library->needed[library->needed_count++] = libc;
    }
    return true;
}

static void run_initializers(struct library *library) {
    const uint64_t *init = find_dynamic(library, DT_INIT);
    const uint64_t *init_array = find_dynamic(library, DT_INIT_ARRAY);
    const uint64_t *init_size = find_dynamic(library, DT_INIT_ARRAYSZ);
    const uint64_t *fini = find_dynamic(library, DT_FINI);
    const uint64_t *fini_array = find_dynamic(library, DT_FINI_ARRAY);
    const uint64_t *fini_size = find_dynamic(library, DT_FINI_ARRAYSZ);

    if (fini)
        library->fini = (void (*)(void))(library->base + *fini);
    if (fini_array && fini_size) {
        library->fini_array = (void (*const *)(void))(library->base + *fini_array);
        library->fini_count = *fini_size / sizeof(void *);
    }

    if (init)
        ((void (*)(void))(library->base + *init))();
    if (init_array && init_size) {
        void (*const *functions)(void) = (const void *)(library->base + *init_array);

        for (size_t i = 0; i < *init_size / sizeof(void *); i++)
            functions[i]();
    }
}

static bool read_headers(struct library *library, int fd, struct elf64_header *header,
                         struct elf64_program_header **headers) {
    if (!read_at(fd, header, sizeof(*header), 0) || memcmp(header->e_ident, "\x7f" "ELF\x02", 5) != 0
        || header->e_type != ET_DYN || header->e_machine != EM_X86_64) {
        set_error("%s: not a 64-bit x86 shared library%s", library->path, "");
        return false;
    }

    size_t size = header->e_phnum * sizeof(**headers);

    *headers = malloc(size ? size : 1);
    if (!*headers || !read_at(fd, *headers, size, header->e_phoff)) {
        set_error("%s: can't read its headers%s", library->path, "");
        return false;
    }
    return true;
}

static struct library *open_library(const char *name, bool global) {
    char path[PATH_MAX];

    if (strchr(name, '/'))
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s/%s", LIBRARY_DIR, name);

    // A name alone also finds what was opened from anywhere else under that name, so
    // that a library and what depends on it share one copy.
    for (struct library *l = libraries; l; l = l->next) {
        if (strcmp(l->path, path) == 0 || (!strchr(name, '/') && library_is_called(l, name))) {
            l->refs++;
            l->global |= global;
            return l;
        }
    }

    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        set_error("%s: %s", path, strerror(errno));
        return NULL;
    }

    struct library *library = calloc(1, sizeof(*library));
    char *copy = strdup(path);
    struct elf64_header header;
    struct elf64_program_header *headers = NULL;

    if (!library || !copy) {
        free(library);
        free(copy);
        close(fd);
        set_error("%s: %s", path, strerror(ENOMEM));
        return NULL;
    }

    library->path = copy;
    library->refs = 1;
    library->global = global;

    bool ok = read_headers(library, fd, &header, &headers) && map_segments(library, fd, &header, headers)
        && find_tables(library);

    close(fd);

    // On the list first, so that libraries needing each other find it.
    if (ok) {
        library->next = libraries;
        libraries = library;
        ok = load_needed(library);
    }

    if (ok) {
        const uint64_t *rela = find_dynamic(library, DT_RELA);
        const uint64_t *rela_size = find_dynamic(library, DT_RELASZ);
        const uint64_t *jmprel = find_dynamic(library, DT_JMPREL);
        const uint64_t *jmprel_size = find_dynamic(library, DT_PLTRELSZ);

        ok = (!rela || !rela_size || relocate(library, (const void *)(library->base + *rela), *rela_size))
            && (!jmprel || !jmprel_size || relocate(library, (const void *)(library->base + *jmprel), *jmprel_size));
    }

    if (!ok) {
        // Nothing has run yet, so there's nothing for it to clean up.
        library->fini = NULL;
        library->fini_count = 0;
        library->refs = 1;
        if (library->map) {
            bool listed = false;

            for (struct library *l = libraries; l; l = l->next)
                listed |= l == library;
            if (listed) {
                close_library(library);
                free(headers);
                return NULL;
            }
            munmap(library->map, library->map_size);
        }
        free(library->path);
        free(library);
        free(headers);
        return NULL;
    }

    protect_segments(library, &header, headers);
    free(headers);
    run_initializers(library);
    return library;
}

// A handle that isn't a library stands for the program.
static char program_handle;

// What's still open gets to clean up, and libc.so has its own streams if the program
// didn't link in stdio.
static void finish_at_exit(void) {
    for (struct library *l = libraries; l; l = l->next) {
        for (size_t i = l->fini_count; i > 0; i--)
            l->fini_array[i - 1]();
        l->fini_count = 0;
        if (l->fini)
            l->fini();
        l->fini = NULL;

        int (*flush)(void *) = library_is_called(l, LIBC_NAME) ? find_in_library(l, "fflush") : NULL;

        if (flush)
            flush(NULL);
    }
}

void *dlopen(const char *path, int flags) {
    LOCKED(&dl_lock);

    static bool registered;

    if (!path)
        return &program_handle;
    if (!registered) {
        registered = true;
        atexit(finish_at_exit);
    }
    return open_library(path, flags & RTLD_GLOBAL);
}

void *dlsym(void *handle, const char *name) {
    LOCKED(&dl_lock);

    void *found;

    if (handle == RTLD_DEFAULT || handle == &program_handle)
        found = resolve(NULL, name);
    else
        found = find_in_tree(handle, name);

    if (!found)
        set_error("undefined symbol %s%s", name, "");
    return found;
}

int dlclose(void *handle) {
    LOCKED(&dl_lock);

    if (handle != &program_handle)
        close_library(handle);
    return 0;
}

char *dlerror(void) {
    if (!has_error)
        return NULL;
    has_error = false;
    return error_text;
}
