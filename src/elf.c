#include <elf.h>
#include <errno_values.h>
#include <mm.h>
#include <serial.h>
#include <string.h>

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

#define MAX_SEGMENTS 16

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

static bool read_exact(const struct vfs_node *file, uint64_t offset, void *buf, size_t len) {
    return vfs_read(file, offset, buf, len) == (int64_t)len;
}

static bool segment_valid(const struct elf64_phdr *ph) {
    return ph->filesz <= ph->memsz && ph->memsz <= LOWER_HALF_END
        && ph->vaddr <= LOWER_HALF_END - ph->memsz;
}

static bool load_segment(const struct vfs_node *file, const struct elf64_phdr *ph) {
    uint64_t flags = PAGE_USER | (ph->flags & PF_W ? PAGE_WRITE : 0)
        | (ph->flags & PF_X ? 0 : PAGE_NO_EXEC);
    uint64_t file_end = ph->vaddr + ph->filesz;
    uint64_t mem_end = ph->vaddr + ph->memsz;

    kprintf("elf: segment %lx-%lx flags %x\n", ph->vaddr, mem_end, ph->flags);

    for (uint64_t page = ph->vaddr & ~(uint64_t)(PAGE_SIZE - 1); page < mem_end; page += PAGE_SIZE) {
        uint64_t phys = mm_alloc_page();

        if (phys == 0 || !mm_map_page(page, phys, flags)) {
            kprintf("elf: failed to map page %lx\n", page);
            return false;
        }

        // Anything past the file-backed range stays zero, which covers .bss.
        uint64_t from = page < ph->vaddr ? ph->vaddr : page;
        uint64_t to = page + PAGE_SIZE < file_end ? page + PAGE_SIZE : file_end;
        if (from < to) {
            uint8_t *dest = (uint8_t *)mm_phys_to_virt(phys) + (from - page);
            if (!read_exact(file, ph->offset + (from - ph->vaddr), dest, to - from))
                return false;
        }
    }
    return true;
}

int elf_load(const struct vfs_node *file, struct elf_image *image) {
    struct elf64_ehdr eh;
    struct elf64_phdr phdrs[MAX_SEGMENTS];

    if (!read_exact(file, 0, &eh, sizeof(eh)) || memcmp(eh.ident, "\x7f" "ELF", 4) != 0) {
        kprintf("elf: not an ELF file\n");
        return -ENOEXEC;
    }
    if (eh.ident[4] != ELFCLASS64 || eh.ident[5] != ELFDATA2LSB || eh.type != ET_EXEC
        || eh.machine != EM_X86_64 || eh.phentsize != sizeof(struct elf64_phdr)
        || eh.phnum > MAX_SEGMENTS) {
        kprintf("elf: not a static x86-64 executable\n");
        return -ENOEXEC;
    }
    if (!read_exact(file, eh.phoff, phdrs, eh.phnum * sizeof(struct elf64_phdr)))
        return -ENOEXEC;

    bool entry_mapped = false;
    image->end = 0;
    for (uint16_t i = 0; i < eh.phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];

        if (ph->type != PT_LOAD)
            continue;
        if (!segment_valid(ph)) {
            kprintf("elf: segment %u is outside the lower half\n", i);
            return -ENOEXEC;
        }
        if (!load_segment(file, ph))
            return -ENOMEM;
        if ((ph->flags & PF_X) && eh.entry >= ph->vaddr && eh.entry - ph->vaddr < ph->memsz)
            entry_mapped = true;
        if (ph->vaddr + ph->memsz > image->end)
            image->end = ph->vaddr + ph->memsz;
    }
    if (!entry_mapped) {
        kprintf("elf: entry point %lx is not in an executable segment\n", eh.entry);
        return -ENOEXEC;
    }

    image->entry = eh.entry;
    return 0;
}
