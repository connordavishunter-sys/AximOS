#pragma once

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define LOWER_HALF_END 0x0000800000000000ULL

#define PAGE_WRITE (1ULL << 1)
#define PAGE_USER (1ULL << 2)
#define PAGE_WRITE_COMBINING (1ULL << 3) // cpu_init sets the PAT up so this bit means it
#define PAGE_SHARED (1ULL << 10) // fork keeps this shared, ignored by the CPU
#define PAGE_COPY_ON_WRITE (1ULL << 9) // read-only until written, ignored by the CPU
#define PAGE_DEVICE (1ULL << 11) // not RAM, so it has no reference count, ignored by the CPU
#define PAGE_INACCESSIBLE (1ULL << 52) // mapped but not present, by mprotect, ignored by the CPU
#define PAGE_NO_EXEC (1ULL << 63)

#define KERNEL_STACK_SIZE (4 * PAGE_SIZE)

bool mm_init(void);

// Returns a zeroed physical page, or 0 when memory is exhausted.
uint64_t mm_alloc_page(void);

// Drops one reference to a page, freeing it when that was the last.
void mm_free_page(uint64_t phys);

// The same, for memory the kernel keeps for itself rather than giving to programs.
uint64_t mm_alloc_kernel_page(void);
void mm_free_kernel_page(uint64_t phys);

// Adds a reference to a page that is about to be mapped somewhere else as well.
void mm_ref_page(uint64_t phys);

uint64_t mm_free_page_count(void);
uint64_t mm_kernel_page_count(void);

// How many user pages an address space has mapped, counting shared ones in full.
uint64_t mm_user_page_count(uint64_t pml4);

// The physical address behind a valid user address. Being the same for every mapping
// of a page, it names a futex even across processes.
uint64_t mm_user_physical(uint64_t virt);

// Changes what's allowed on one page, returning false if it isn't mapped.
bool mm_protect_page(uint64_t virt, bool read, bool write, bool exec);
uint64_t mm_total_page_count(void);

void *mm_phys_to_virt(uint64_t phys);

// Like mm_map_device, but writes are gathered up, which is far faster for pixels.
void *mm_map_framebuffer(uint64_t phys, size_t size);
uint64_t mm_virt_to_phys(const void *virt);

// Maps a page in the active address space. Fails if virt is already mapped.
bool mm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Unmaps a page and frees the physical page behind it.
void mm_unmap_page(uint64_t virt);

// The same for a range of pages, some of which may not be mapped, but cheaper.
void mm_unmap_range(uint64_t start, uint64_t end);

// Address spaces are named by the physical address of their top-level table.
// All of them share the kernel's higher half mappings.
uint64_t mm_kernel_address_space(void);
uint64_t mm_active_address_space(void);
void mm_switch_address_space(uint64_t pml4);

// These return 0 when memory is exhausted. A clone shares its pages with the
// original until either one writes to them.
uint64_t mm_create_address_space(void);
uint64_t mm_clone_active_address_space(void);

// Frees all user pages and tables. pml4 must not be the active address space.
void mm_destroy_address_space(uint64_t pml4);

// Gives the page at virt a private writable copy, if a write fault there was
// only because the page is shared. Returns false for a real fault or no memory.
bool mm_handle_write_fault(uint64_t virt);

// Checks that a whole range is mapped and accessible from ring 3. A writable
// range also stops sharing its pages, so the kernel can't fault writing to it.
bool mm_user_range_ok(uint64_t virt, size_t len, bool writable);

// Copies a NUL-terminated user string. Returns its length, or a negative errno
// if it is unmapped or doesn't fit in max bytes including the terminator.
int64_t mm_copy_user_string(char *dest, uint64_t src, size_t max);

// Kernel stacks have an unmapped page below them, so an overflow faults instead
// of corrupting a neighbour. These return and take the top of the stack.
uint64_t mm_alloc_kernel_stack(void);
void mm_free_kernel_stack(uint64_t top);

// Maps device registers uncached, returning 0 when memory is exhausted.
void *mm_map_device(uint64_t phys, size_t size);
