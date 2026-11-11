#include <elf.h>
#include <elf_types.h>
#include <errno_values.h>
#include <mm.h>
#include <serial.h>
#include <string.h>

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define MAX_SEGMENTS 16

// A program built position independent goes where a fixed one would, and its
// dynamic linker well above the heap's limit and below the mappings.
#define PROGRAM_BASE 0x400000UL
#define INTERPRETER_BASE 0x0000500000000000UL

// Read-only segments are read once and their pages shared by every process running
// the file, so libc.so and each program's code are in memory once. The cache holds a
// reference of its own to each page, keeping it after the last process exits, until
// the file changes, its slot is needed or memory runs out.
#define CACHE_SLOTS 128
#define CACHE_MAX_PAGES (PAGE_SIZE / sizeof(uint64_t))

struct cached_segment {
    struct vfs_node file;
    uint64_t offset, vaddr, filesz, memsz;
    uint64_t count;
    uint64_t *pages; // a page of its own, so dropping it never needs the heap
    uint64_t used;   // when it was last mapped, so the oldest goes first
};

static struct cached_segment cache[CACHE_SLOTS];
static struct cached_segment *mapping; // what reclaiming mustn't drop while it's being mapped
static uint64_t cache_clock;
static uint64_t cache_changes; // so a load that slept knows a file changed meanwhile

struct loaded {
    uint64_t entry;
    uint64_t end;
    uint64_t phdr;
    uint16_t phnum;
};

static bool read_exact(const struct vfs_node *file, uint64_t offset, void *buf, size_t len) {
    return vfs_read(file, offset, buf, len) == (int64_t)len;
}

static bool segment_valid(const struct elf64_program_header *ph, uint64_t base) {
    return ph->p_filesz <= ph->p_memsz && ph->p_memsz <= LOWER_HALF_END && base <= LOWER_HALF_END
        && ph->p_vaddr <= LOWER_HALF_END - base - ph->p_memsz;
}

static uint64_t first_page(const struct elf64_program_header *ph, uint64_t base) {
    return (base + ph->p_vaddr) & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t page_count(const struct elf64_program_header *ph) {
    return (ph->p_vaddr % PAGE_SIZE + ph->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
}

// Fills the nth page of a segment from the file. Anything past the file-backed
// range stays zero, which covers .bss.
static bool read_page(const struct vfs_node *file, const struct elf64_program_header *ph, uint64_t n, uint64_t phys) {
    uint64_t start = ph->p_vaddr;
    uint64_t page = (start & ~(uint64_t)(PAGE_SIZE - 1)) + n * PAGE_SIZE;
    uint64_t from = page < start ? start : page;
    uint64_t to = page + PAGE_SIZE < start + ph->p_filesz ? page + PAGE_SIZE : start + ph->p_filesz;

    return from >= to || read_exact(file, ph->p_offset + (from - start),
                                    (uint8_t *)mm_phys_to_virt(phys) + (from - page), to - from);
}

static bool map_failed(uint64_t page) {
    kprintf("elf: failed to map page %lx\n", page);
    return false;
}

static bool load_private(const struct vfs_node *file, const struct elf64_program_header *ph, uint64_t base,
                         uint64_t flags) {
    for (uint64_t n = 0, page = first_page(ph, base); n < page_count(ph); n++, page += PAGE_SIZE) {
        uint64_t phys = mm_alloc_page();

        if (phys == 0 || !mm_map_page(page, phys, flags)) {
            if (phys)
                mm_free_page(phys);
            return map_failed(page);
        }
        if (!read_page(file, ph, n, phys))
            return false;
    }
    return true;
}

static bool cached_is(const struct cached_segment *c, const struct vfs_node *file, const struct elf64_program_header *ph) {
    return c->pages && vfs_same_file(&c->file, file) && c->file.location == file->location
        && c->file.size == file->size && c->file.mtime == file->mtime && c->offset == ph->p_offset
        && c->vaddr == ph->p_vaddr && c->filesz == ph->p_filesz && c->memsz == ph->p_memsz;
}

static void drop(struct cached_segment *c) {
    for (uint64_t i = 0; i < c->count; i++)
        mm_free_page(c->pages[i]);
    mm_free_kernel_page(mm_virt_to_phys(c->pages));
    c->pages = NULL;
}

// Reads a segment into the cache, or returns NULL if it can't be cached.
static struct cached_segment *cache_segment(const struct vfs_node *file, const struct elf64_program_header *ph) {
    uint64_t changes = cache_changes;
    uint64_t list = mm_alloc_kernel_page();

    if (list == 0)
        return NULL;

    struct cached_segment loaded = {
        .file = *file, .offset = ph->p_offset, .vaddr = ph->p_vaddr, .filesz = ph->p_filesz,
        .memsz = ph->p_memsz, .pages = mm_phys_to_virt(list),
    };

    for (; loaded.count < page_count(ph); loaded.count++) {
        uint64_t phys = mm_alloc_page();

        if (phys == 0 || !read_page(file, ph, loaded.count, phys)) {
            if (phys)
                mm_free_page(phys);
            drop(&loaded);
            return NULL;
        }
        loaded.pages[loaded.count] = phys;
    }

    // Reading slept, and the file may have changed or someone else cached it meanwhile.
    struct cached_segment *slot = &cache[0];

    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (changes != cache_changes || cached_is(&cache[i], file, ph)) {
            drop(&loaded);
            return changes != cache_changes ? NULL : &cache[i];
        }
        if (!cache[i].pages ? slot->pages != NULL : slot->pages && cache[i].used < slot->used)
            slot = &cache[i];
    }
    if (slot->pages)
        drop(slot);
    *slot = loaded;
    return slot;
}

static struct cached_segment *find_cached(const struct vfs_node *file, const struct elf64_program_header *ph) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cached_is(&cache[i], file, ph))
            return &cache[i];
    }
    return page_count(ph) <= CACHE_MAX_PAGES ? cache_segment(file, ph) : NULL;
}

static bool load_segment(const struct vfs_node *file, const struct elf64_program_header *ph, uint64_t base) {
    uint64_t flags = PAGE_USER | (ph->p_flags & PF_W ? PAGE_WRITE : 0) | (ph->p_flags & PF_X ? 0 : PAGE_NO_EXEC);
    struct cached_segment *c = ph->p_flags & PF_W ? NULL : find_cached(file, ph);

    if (!c)
        return load_private(file, ph, base, flags);

    // Mapping can allocate page tables, which can reclaim the cache.
    c->used = ++cache_clock;
    mapping = c;
    for (uint64_t n = 0, page = first_page(ph, base); n < c->count; n++, page += PAGE_SIZE) {
        mm_ref_page(c->pages[n]);
        if (!mm_map_page(page, c->pages[n], flags)) {
            mm_free_page(c->pages[n]);
            mapping = NULL;
            return map_failed(page);
        }
    }
    mapping = NULL;
    return true;
}

void elf_forget(const struct vfs_node *file) {
    cache_changes++;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].pages && vfs_same_file(&cache[i].file, file))
            drop(&cache[i]);
    }
}

bool elf_reclaim(void) {
    bool freed = false;

    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].pages && &cache[i] != mapping) {
            drop(&cache[i]);
            freed = true;
        }
    }
    return freed;
}

// Where the program headers are once loaded: PT_PHDR says, or else the segment
// holding that part of the file.
static uint64_t find_phdr(const struct elf64_header *eh, const struct elf64_program_header *phdrs, uint64_t base) {
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type == PT_PHDR)
            return base + phdrs[i].p_vaddr;
    }
    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_program_header *ph = &phdrs[i];

        if (ph->p_type == PT_LOAD && eh->e_phoff >= ph->p_offset && eh->e_phoff - ph->p_offset < ph->p_filesz)
            return base + ph->p_vaddr + (eh->e_phoff - ph->p_offset);
    }
    return 0;
}

// Loads one file. interpreter gets the PT_INTERP path if there's one and it's wanted.
static int load_file(const struct vfs_node *file, bool is_interpreter, struct loaded *out,
                     char interpreter[PATH_MAX]) {
    struct elf64_header eh;
    struct elf64_program_header phdrs[MAX_SEGMENTS];

    if (!read_exact(file, 0, &eh, sizeof(eh)) || memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0) {
        kprintf("elf: not an ELF file\n");
        return -ENOEXEC;
    }
    if (eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB || eh.e_machine != EM_X86_64
        || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) || (is_interpreter && eh.e_type != ET_DYN)
        || eh.e_phentsize != sizeof(struct elf64_program_header) || eh.e_phnum > MAX_SEGMENTS) {
        kprintf("elf: not an x86-64 program\n");
        return -ENOEXEC;
    }
    if (!read_exact(file, eh.e_phoff, phdrs, eh.e_phnum * sizeof(*phdrs)))
        return -ENOEXEC;

    uint64_t base = eh.e_type == ET_EXEC ? 0 : is_interpreter ? INTERPRETER_BASE : PROGRAM_BASE;
    bool entry_mapped = false;

    out->end = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        const struct elf64_program_header *ph = &phdrs[i];

        if (ph->p_type == PT_INTERP && interpreter) {
            if (ph->p_filesz == 0 || ph->p_filesz > PATH_MAX || !read_exact(file, ph->p_offset, interpreter, ph->p_filesz)
                || interpreter[ph->p_filesz - 1] != '\0')
                return -ENOEXEC;
        }
        if (ph->p_type != PT_LOAD)
            continue;
        if (!segment_valid(ph, base)) {
            kprintf("elf: segment %u is outside the lower half\n", i);
            return -ENOEXEC;
        }
        if (!load_segment(file, ph, base))
            return -ENOMEM;
        if ((ph->p_flags & PF_X) && eh.e_entry >= ph->p_vaddr && eh.e_entry - ph->p_vaddr < ph->p_memsz)
            entry_mapped = true;
        if (base + ph->p_vaddr + ph->p_memsz > out->end)
            out->end = base + ph->p_vaddr + ph->p_memsz;
    }
    if (!entry_mapped) {
        kprintf("elf: entry point %lx is not in an executable segment\n", eh.e_entry);
        return -ENOEXEC;
    }

    out->entry = base + eh.e_entry;
    out->phdr = find_phdr(&eh, phdrs, base);
    out->phnum = eh.e_phnum;
    return 0;
}

int elf_load(const struct vfs_node *file, struct elf_image *image) {
    char interpreter[PATH_MAX] = "";
    struct loaded program, linker;
    struct vfs_node linker_file;

    int status = load_file(file, false, &program, interpreter);

    if (status != 0)
        return status;

    *image = (struct elf_image){
        .entry = program.entry,
        .end = program.end,
        .program_entry = program.entry,
        .phdr = program.phdr,
        .phnum = program.phnum,
    };
    if (!interpreter[0])
        return 0;

    status = vfs_lookup("/", interpreter, &linker_file);
    if (status != 0 || linker_file.is_dir) {
        kprintf("elf: no dynamic linker %s\n", interpreter);
        return -ENOENT;
    }
    status = load_file(&linker_file, true, &linker, NULL);
    if (status != 0)
        return status;

    image->entry = linker.entry;
    image->interpreter_base = INTERPRETER_BASE;
    return 0;
}
