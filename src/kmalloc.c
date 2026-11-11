#include <kmalloc.h>
#include <mm.h>
#include <stdint.h>
#include <string.h>

// Pages are split into equal objects, one size per page. The direct map makes a
// page's address its slab header, and empty pages go back to the page allocator.

#define CLASS_COUNT 8

struct slab {
    struct slab *prev;
    struct slab *next;
    void *free_objects;
    uint32_t in_use;
    uint32_t class_index;
};

_Static_assert(sizeof(struct slab) <= 32, "slab header must leave objects aligned");

static const size_t class_sizes[CLASS_COUNT] = {32, 64, 128, 256, 512, 1024, 2032, KMALLOC_MAX};

// Slabs that have free objects, by size.
static struct slab *partial[CLASS_COUNT];

static void list_add(struct slab *slab) {
    struct slab **head = &partial[slab->class_index];

    slab->prev = NULL;
    slab->next = *head;
    if (*head)
        (*head)->prev = slab;
    *head = slab;
}

static void list_remove(struct slab *slab) {
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        partial[slab->class_index] = slab->next;
    if (slab->next)
        slab->next->prev = slab->prev;
}

static struct slab *new_slab(int index) {
    uint64_t phys = mm_alloc_kernel_page();
    if (phys == 0)
        return NULL;

    struct slab *slab = mm_phys_to_virt(phys);
    size_t size = class_sizes[index];

    slab->class_index = index;
    for (size_t at = 32; at + size <= PAGE_SIZE; at += size) {
        void **object = (void **)((char *)slab + at);

        *object = slab->free_objects;
        slab->free_objects = object;
    }
    list_add(slab);
    return slab;
}

void *kmalloc(size_t size) {
    int index = 0;

    while (index < CLASS_COUNT && class_sizes[index] < size)
        index++;
    if (index == CLASS_COUNT)
        return NULL;

    struct slab *slab = partial[index] ? partial[index] : new_slab(index);
    if (!slab)
        return NULL;

    void **object = slab->free_objects;

    slab->free_objects = *object;
    slab->in_use++;
    if (!slab->free_objects)
        list_remove(slab);

    memset(object, 0, class_sizes[index]);
    return object;
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    struct slab *slab = (struct slab *)((uint64_t)ptr & ~(uint64_t)(PAGE_SIZE - 1));

    if (!slab->free_objects)
        list_add(slab);

    *(void **)ptr = slab->free_objects;
    slab->free_objects = ptr;

    if (--slab->in_use == 0) {
        list_remove(slab);
        mm_free_kernel_page(mm_virt_to_phys(slab));
    }
}
