#include <limine.h>
#include <mm.h>
#include <serial.h>
#include <trace.h>

// Names come from the kernel's own symbol table, in the file Limine loaded it from,
// which stays where it is as that memory is never handed out.

#define SECTION_SYMBOLS 2
#define SYMBOL_FUNCTION 2
#define MAX_FRAMES 16
#define KERNEL_HALF 0xFFFF800000000000ULL

struct section {
    uint32_t name, type;
    uint64_t flags, address, offset, size;
    uint32_t link, info;
    uint64_t align, entry_size;
};

struct symbol {
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_file_request file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0,
};

// The function an address is in, and how far into it, or NULL.
static const char *name_of(uint64_t address, uint64_t *offset) {
    if (!file_request.response)
        return NULL;

    const uint8_t *elf = file_request.response->executable_file->address;
    const struct section *sections = (const struct section *)(elf + *(const uint64_t *)(elf + 0x28));
    uint16_t count = *(const uint16_t *)(elf + 0x3C);
    const char *best = NULL;

    for (uint16_t i = 0; i < count; i++) {
        if (sections[i].type != SECTION_SYMBOLS)
            continue;

        const struct symbol *symbols = (const struct symbol *)(elf + sections[i].offset);
        const char *names = (const char *)(elf + sections[sections[i].link].offset);

        for (uint64_t j = 0; j < sections[i].size / sizeof(struct symbol); j++) {
            const struct symbol *s = &symbols[j];

            if ((s->info & 0xF) == SYMBOL_FUNCTION && s->value <= address && address - s->value < (s->size ? s->size : 1)) {
                best = names + s->name;
                *offset = address - s->value;
            }
        }
    }
    return best;
}

static void print_address(uint64_t address) {
    uint64_t offset;
    const char *name = name_of(address, &offset);

    if (name)
        kprintf("  %lx %s+%lx\n", address, name, offset);
    else
        kprintf("  %lx\n", address);
}

void trace_print(uint64_t rip, uint64_t rbp) {
    uint64_t top = rbp + KERNEL_STACK_SIZE;

    kprintf("trace:\n");
    if (rip)
        print_address(rip);
    // Each frame starts with the one below's, then where to return to. Anything
    // that doesn't look like a kernel stack going up ends it.
    for (int i = 0; i < MAX_FRAMES && rbp >= KERNEL_HALF && rbp % 8 == 0 && rbp < top; i++) {
        const uint64_t *frame = (const uint64_t *)rbp;

        if (frame[1] < KERNEL_HALF)
            break;
        print_address(frame[1]);
        if (frame[0] <= rbp)
            break;
        rbp = frame[0];
    }
}
