#include <cpu.h>
#include <elf.h>
#include <errno_values.h>
#include <limine.h>
#include <mm.h>
#include <process.h>
#include <serial.h>
#include <string.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_NO_CACHE (1ULL << 4)
#define PTE_HUGE (1ULL << 7)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

#define USER_PML4_ENTRIES 256

// Kernel stacks and device registers live here, away from the direct map. Its
// top-level entry exists from boot, so every address space shares what's added.
#define KERNEL_VM_BASE 0xFFFFC00000000000ULL
#define STACK_GUARD_SIZE PAGE_SIZE

// How many pages mm_unmap_range unmaps between telling the other CPUs.
#define UNMAP_BATCH 64

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

static uint64_t hhdm_offset;
static uint64_t kernel_pml4;
static struct limine_memmap_response *memmap;

// Freed pages form a linked list through their own first bytes. Memory that
// has never been allocated is handed out by bumping through the memory map.
static uint64_t free_list;
static uint64_t free_pages;
static uint64_t total_pages;
static uint64_t kernel_pages; // the image, the reference counts and kernel allocations
static uint64_t region;
static uint64_t region_used;

extern char __kernel_start[], __kernel_end[]; // from the linker script

// How many address spaces use each physical page.
static uint16_t *page_refs;

static uint64_t kernel_vm_next = KERNEL_VM_BASE;

// Stacks keep their pages when freed. Unmapping them would need every CPU's TLB
// flushed before the addresses could be used again.
static uint64_t free_stacks;

// Takes room for the reference counts from the first region big enough.
static bool init_page_refs(void) {
    uint64_t highest = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE && entry->base + entry->length > highest)
            highest = entry->base + entry->length;
    }

    uint64_t bytes = highest / PAGE_SIZE * sizeof(uint16_t);
    uint64_t rounded = (bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length < rounded)
            continue;

        page_refs = mm_phys_to_virt(entry->base);
        memset(page_refs, 0, rounded);
        entry->base += rounded;
        entry->length -= rounded;
        kernel_pages += rounded / PAGE_SIZE;
        return true;
    }
    return false;
}

bool mm_init(void) {
    if (!hhdm_request.response || !memmap_request.response)
        return false;

    hhdm_offset = hhdm_request.response->offset;
    memmap = memmap_request.response;
    kernel_pml4 = mm_active_address_space();

    if (!init_page_refs())
        return false;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        if (memmap->entries[i]->type == LIMINE_MEMMAP_USABLE)
            free_pages += memmap->entries[i]->length / PAGE_SIZE;
    }
    kernel_pages += (__kernel_end - __kernel_start + PAGE_SIZE - 1) / PAGE_SIZE;
    total_pages = free_pages + kernel_pages;
    kprintf("mm: hhdm offset %lx, %lu pages free in %lu memmap entries\n",
            hhdm_offset, free_pages, memmap->entry_count);

    uint64_t *pml4 = mm_phys_to_virt(kernel_pml4);
    uint64_t table = mm_alloc_kernel_page();
    if (table == 0)
        return false;

    pml4[KERNEL_VM_BASE >> 39 & 0x1FF] = table | PTE_PRESENT | PAGE_WRITE;
    return true;
}

void *mm_phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

uint64_t mm_virt_to_phys(const void *virt) {
    return (uint64_t)virt - hhdm_offset;
}

static uint64_t next_unused_page(void) {
    for (; region < memmap->entry_count; region++, region_used = 0) {
        struct limine_memmap_entry *entry = memmap->entries[region];

        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length - region_used < PAGE_SIZE)
            continue;

        uint64_t phys = entry->base + region_used;
        region_used += PAGE_SIZE;

        // Physical 0 is the failure value, so it can't be handed out.
        if (phys != 0)
            return phys;
    }
    return 0;
}

uint64_t mm_alloc_page(void) {
    if (free_pages == 0 && elf_reclaim())
        return mm_alloc_page();

    uint64_t phys = free_list;

    if (phys)
        free_list = *(uint64_t *)mm_phys_to_virt(phys);
    else
        phys = next_unused_page();

    if (phys) {
        memset(mm_phys_to_virt(phys), 0, PAGE_SIZE);
        page_refs[phys / PAGE_SIZE] = 1;
        free_pages--;
    }
    return phys;
}

void mm_free_page(uint64_t phys) {
    if (--page_refs[phys / PAGE_SIZE] > 0)
        return;

    *(uint64_t *)mm_phys_to_virt(phys) = free_list;
    free_list = phys;
    free_pages++;
}

uint64_t mm_alloc_kernel_page(void) {
    uint64_t phys = mm_alloc_page();

    if (phys)
        kernel_pages++;
    return phys;
}

void mm_free_kernel_page(uint64_t phys) {
    kernel_pages--;
    mm_free_page(phys);
}

void mm_ref_page(uint64_t phys) {
    page_refs[phys / PAGE_SIZE]++;
}

uint64_t mm_free_page_count(void) {
    return free_pages;
}

uint64_t mm_kernel_page_count(void) {
    return kernel_pages;
}

uint64_t mm_total_page_count(void) {
    return total_pages;
}

uint64_t mm_kernel_address_space(void) {
    return kernel_pml4;
}

uint64_t mm_active_address_space(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE_ADDR;
}

void mm_switch_address_space(uint64_t pml4) {
    this_cpu()->address_space = pml4;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4));
}

// Returns the page table entry for virt. Tables in the lower half stay
// user-accessible, leaving the final entry to decide who can access the page.
static uint64_t *walk(uint64_t virt, bool create) {
    uint64_t *table = mm_phys_to_virt(mm_active_address_space());

    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *entry = &table[virt >> shift & 0x1FF];

        if (*entry & PTE_HUGE)
            return NULL;
        if (!(*entry & PTE_PRESENT)) {
            uint64_t next = create ? mm_alloc_kernel_page() : 0;

            if (next == 0)
                return NULL;
            *entry = next | PTE_PRESENT | PAGE_WRITE | (virt < LOWER_HALF_END ? PAGE_USER : 0);
        }
        table = mm_phys_to_virt(*entry & PTE_ADDR);
    }
    return &table[virt >> 12 & 0x1FF];
}

bool mm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *entry = walk(virt, true);

    if (!entry || (*entry & (PTE_PRESENT | PAGE_INACCESSIBLE)))
        return false;

    *entry = phys | PTE_PRESENT | flags;
    __asm__ volatile("invlpg (%0)" : : "r"(virt));
    return true;
}

void mm_unmap_page(uint64_t virt) {
    uint64_t *entry = walk(virt, false);

    if (!entry || !(*entry & (PTE_PRESENT | PAGE_INACCESSIBLE)))
        return;

    uint64_t old = *entry;

    *entry = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt));

    // Another thread could still write to it until this is done.
    cpu_flush_others();
    if (!(old & PAGE_DEVICE))
        mm_free_page(old & PTE_ADDR);
}

void mm_unmap_range(uint64_t start, uint64_t end) {
    uint64_t waiting[UNMAP_BATCH];
    int count = 0;

    for (uint64_t virt = start; virt < end; virt += PAGE_SIZE) {
        uint64_t *entry = walk(virt, false);

        if (entry && (*entry & (PTE_PRESENT | PAGE_INACCESSIBLE))) {
            if (!(*entry & PAGE_DEVICE))
                waiting[count++] = *entry & PTE_ADDR;
            *entry = 0;
            __asm__ volatile("invlpg (%0)" : : "r"(virt));
        }
        if (count == UNMAP_BATCH || virt + PAGE_SIZE >= end) {
            cpu_flush_others();
            while (count > 0)
                mm_free_page(waiting[--count]);
        }
    }
}

// level 1 tables point at data pages, higher levels at more tables.
static void free_table(uint64_t phys, int level) {
    uint64_t *table = mm_phys_to_virt(phys);

    for (int i = 0; i < 512; i++) {
        if (!(table[i] & (PTE_PRESENT | PAGE_INACCESSIBLE)))
            continue;
        if (level > 1)
            free_table(table[i] & PTE_ADDR, level - 1);
        else if (!(table[i] & PAGE_DEVICE))
            mm_free_page(table[i] & PTE_ADDR);
    }
    mm_free_kernel_page(phys);
}

static uint64_t count_table(uint64_t phys, int level) {
    uint64_t *table = mm_phys_to_virt(phys);
    uint64_t count = 0;

    for (int i = 0; i < 512; i++) {
        if (!(table[i] & (PTE_PRESENT | PAGE_INACCESSIBLE)))
            continue;
        count += level > 1 ? count_table(table[i] & PTE_ADDR, level - 1) : 1;
    }
    return count;
}

uint64_t mm_user_page_count(uint64_t pml4) {
    uint64_t *table = mm_phys_to_virt(pml4);
    uint64_t count = 0;

    for (int i = 0; i < USER_PML4_ENTRIES; i++) {
        if (table[i] & PTE_PRESENT)
            count += count_table(table[i] & PTE_ADDR, 3);
    }
    return count;
}

uint64_t mm_create_address_space(void) {
    uint64_t pml4 = mm_alloc_kernel_page();

    if (pml4) {
        uint64_t *dest = mm_phys_to_virt(pml4);
        uint64_t *src = mm_phys_to_virt(kernel_pml4);

        for (int i = USER_PML4_ENTRIES; i < 512; i++)
            dest[i] = src[i];
    }
    return pml4;
}

void mm_destroy_address_space(uint64_t pml4) {
    uint64_t *table = mm_phys_to_virt(pml4);

    for (int i = 0; i < USER_PML4_ENTRIES; i++) {
        if (table[i] & PTE_PRESENT)
            free_table(table[i] & PTE_ADDR, 3);
    }
    mm_free_kernel_page(pml4);
}

static uint64_t clone_table(uint64_t phys, int level) {
    uint64_t copy = mm_alloc_kernel_page();
    if (copy == 0)
        return 0;

    uint64_t *src = mm_phys_to_virt(phys);
    uint64_t *dest = mm_phys_to_virt(copy);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & (PTE_PRESENT | PAGE_INACCESSIBLE)))
            continue;

        // Data pages are shared. Writable ones become read-only on both sides,
        // and whichever writes first gets its own copy.
        if (level == 1) {
            if ((src[i] & PAGE_WRITE) && !(src[i] & PAGE_SHARED))
                src[i] = (src[i] & ~PAGE_WRITE) | PAGE_COPY_ON_WRITE;

            if (!(src[i] & PAGE_DEVICE))
                page_refs[(src[i] & PTE_ADDR) / PAGE_SIZE]++;
            dest[i] = src[i];
            continue;
        }

        uint64_t child = clone_table(src[i] & PTE_ADDR, level - 1);
        if (child == 0) {
            free_table(copy, level);
            return 0;
        }
        dest[i] = child | (src[i] & ~PTE_ADDR);
    }
    return copy;
}

uint64_t mm_clone_active_address_space(void) {
    uint64_t pml4 = mm_create_address_space();
    if (pml4 == 0)
        return 0;

    uint64_t *src = mm_phys_to_virt(mm_active_address_space());
    uint64_t *dest = mm_phys_to_virt(pml4);

    for (int i = 0; i < USER_PML4_ENTRIES; i++) {
        if (!(src[i] & PTE_PRESENT))
            continue;

        uint64_t child = clone_table(src[i] & PTE_ADDR, 3);
        if (child == 0) {
            mm_destroy_address_space(pml4);
            return 0;
        }
        dest[i] = child | (src[i] & ~PTE_ADDR);
    }

    // The original's pages just became read-only, and its other threads have to know
    // before the child can see what they write.
    mm_switch_address_space(mm_active_address_space());
    cpu_flush_others();
    return pml4;
}

bool mm_handle_write_fault(uint64_t virt) {
    if (virt >= LOWER_HALF_END)
        return false;

    uint64_t page = virt & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *entry = walk(page, false);

    if (!entry || !(*entry & PTE_PRESENT))
        return false;

    // Another thread got here first, and this CPU only remembered the page as read-only.
    if (*entry & PAGE_WRITE) {
        __asm__ volatile("invlpg (%0)" : : "r"(page));
        return true;
    }
    if (!(*entry & PAGE_COPY_ON_WRITE))
        return false;

    uint64_t phys = *entry & PTE_ADDR;
    uint64_t flags = (*entry & ~PTE_ADDR & ~PAGE_COPY_ON_WRITE) | PAGE_WRITE;
    uint64_t replaced = 0;

    if (page_refs[phys / PAGE_SIZE] > 1) {
        uint64_t copy = mm_alloc_page();
        if (copy == 0)
            return false;

        memcpy(mm_phys_to_virt(copy), mm_phys_to_virt(phys), PAGE_SIZE);
        replaced = phys;
        phys = copy;
    }

    *entry = phys | flags;
    __asm__ volatile("invlpg (%0)" : : "r"(page));
    if (replaced) {
        cpu_flush_others();
        process_move_futex_waiters(replaced, phys);
        mm_free_page(replaced);
    }
    return true;
}

bool mm_protect_page(uint64_t virt, bool read, bool write, bool exec) {
    uint64_t *entry = walk(virt, false);

    if (!entry || !(*entry & (PTE_PRESENT | PAGE_INACCESSIBLE)) || !(*entry & PAGE_USER))
        return false;

    uint64_t phys = *entry & PTE_ADDR;
    uint64_t flags = *entry & ~PTE_ADDR & ~(PTE_PRESENT | PAGE_INACCESSIBLE | PAGE_WRITE | PAGE_COPY_ON_WRITE | PAGE_NO_EXEC);

    flags |= read || write || exec ? PTE_PRESENT : PAGE_INACCESSIBLE;
    if (!exec)
        flags |= PAGE_NO_EXEC;

    // A page fork shares has to go on being copied before it's changed.
    if (write)
        flags |= (*entry & PAGE_SHARED) || (*entry & PAGE_DEVICE) || page_refs[phys / PAGE_SIZE] == 1 ? PAGE_WRITE : PAGE_COPY_ON_WRITE;

    *entry = phys | flags;
    __asm__ volatile("invlpg (%0)" : : "r"(virt));
    return true;
}

uint64_t mm_user_physical(uint64_t virt) {
    uint64_t *entry = walk(virt & ~(uint64_t)(PAGE_SIZE - 1), false);

    return entry ? (*entry & PTE_ADDR) + virt % PAGE_SIZE : 0;
}

bool mm_user_range_ok(uint64_t virt, size_t len, bool writable) {
    if (virt >= LOWER_HALF_END || len > LOWER_HALF_END - virt)
        return false;
    if (len == 0)
        return true;

    uint64_t last = (virt + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t page = virt & ~(uint64_t)(PAGE_SIZE - 1); page <= last; page += PAGE_SIZE) {
        uint64_t *entry = walk(page, false);

        if ((!entry || !(*entry & (PTE_PRESENT | PAGE_INACCESSIBLE))) && process_fault_in(page))
            entry = walk(page, false);
        if (!entry || !(*entry & PTE_PRESENT) || !(*entry & PAGE_USER))
            return false;
        if (writable && !(*entry & PAGE_WRITE) && !mm_handle_write_fault(page))
            return false;
    }
    return true;
}

int64_t mm_copy_user_string(char *dest, uint64_t src, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || (src + i) % PAGE_SIZE == 0) && !mm_user_range_ok(src + i, 1, false))
            return -EFAULT;

        dest[i] = *(const char *)(src + i);
        if (dest[i] == '\0')
            return i;
    }
    return -ENAMETOOLONG;
}

uint64_t mm_alloc_kernel_stack(void) {
    if (free_stacks) {
        uint64_t top = free_stacks;

        free_stacks = *(uint64_t *)(top - sizeof(uint64_t));
        return top;
    }

    uint64_t bottom = kernel_vm_next + STACK_GUARD_SIZE;

    for (uint64_t at = 0; at < KERNEL_STACK_SIZE; at += PAGE_SIZE) {
        uint64_t phys = mm_alloc_kernel_page();

        // Pages already mapped stay out of use, which only a machine that has
        // run out of memory will ever notice.
        if (phys == 0 || !mm_map_page(bottom + at, phys, PAGE_WRITE | PAGE_NO_EXEC))
            return 0;
    }

    kernel_vm_next = bottom + KERNEL_STACK_SIZE;
    return kernel_vm_next;
}

void mm_free_kernel_stack(uint64_t top) {
    *(uint64_t *)(top - sizeof(uint64_t)) = free_stacks;
    free_stacks = top;
}

static void *map_kernel_range(uint64_t phys, size_t size, uint64_t flags) {
    uint64_t offset = phys % PAGE_SIZE;
    uint64_t base = kernel_vm_next;
    uint64_t pages = (offset + size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t *entry = walk(base + i * PAGE_SIZE, true);
        if (!entry)
            return NULL;

        *entry = (phys - offset + i * PAGE_SIZE) | PTE_PRESENT | PAGE_WRITE | PAGE_NO_EXEC | flags;
    }

    kernel_vm_next += pages * PAGE_SIZE;
    return (void *)(base + offset);
}

void *mm_map_device(uint64_t phys, size_t size) {
    return map_kernel_range(phys, size, PTE_NO_CACHE | PTE_WRITE_THROUGH);
}

void *mm_map_framebuffer(uint64_t phys, size_t size) {
    return map_kernel_range(phys, size, PAGE_WRITE_COMBINING);
}
