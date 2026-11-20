#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lock.h"
#include "syscall.h"

#define ALIGNMENT 16
#define MIN_SPLIT (sizeof(struct block) + ALIGNMENT)
#define BINS 48

// Blocks sit back to back on the heap, linked in address order so neighbours
// can be merged. The header size keeps payloads 16-byte aligned.
struct block {
    size_t size;
    struct block *prev;
    struct block *next;
    size_t free;
};

// A free block is also on the list for its size, from bins[n] for sizes from 2^n
// up to 2^(n+1), linked through its payload, which is never smaller than this.
struct links {
    struct block *next, *prev;
};

#define HEAP_LOCKED LOCKED(&__heap_lock)

static struct block *head;
static struct block *tail;
static struct block *bins[BINS];

static struct links *links(struct block *b) {
    return (struct links *)(b + 1);
}

static int bin_of(size_t size) {
    int bin = 63 - __builtin_clzl(size);

    return bin < BINS ? bin : BINS - 1;
}

static void add_free(struct block *b) {
    struct block **bin = &bins[bin_of(b->size)];

    b->free = true;
    *links(b) = (struct links){.next = *bin};
    if (*bin)
        links(*bin)->prev = b;
    *bin = b;
}

static void take_free(struct block *b) {
    struct links *l = links(b);

    if (l->prev)
        links(l->prev)->next = l->next;
    else
        bins[bin_of(b->size)] = l->next;
    if (l->next)
        links(l->next)->prev = l->prev;
    b->free = false;
}

static char *heap_end(void) {
    return (char *)syscall3(SYS_BRK, 0, 0, 0);
}

static bool set_heap_end(char *end) {
    return (char *)syscall3(SYS_BRK, (long)end, 0, 0) == end;
}

// Absorbs the block after b, which mustn't be on a free list.
static void merge_next(struct block *b) {
    struct block *next = b->next;

    b->size += sizeof(struct block) + next->size;
    b->next = next->next;
    if (b->next)
        b->next->prev = b;
    else
        tail = b;
}

// Gives what b doesn't need of its size back as a free block.
static void split(struct block *b, size_t size) {
    if (b->size < size + MIN_SPLIT)
        return;

    struct block *rest = (struct block *)((char *)(b + 1) + size);
    rest->size = b->size - size - sizeof(struct block);
    rest->prev = b;
    rest->next = b->next;
    if (rest->next)
        rest->next->prev = rest;
    else
        tail = rest;
    b->next = rest;
    b->size = size;

    if (rest->next && rest->next->free) {
        take_free(rest->next);
        merge_next(rest);
    }
    add_free(rest);
}

static struct block *extend_heap(size_t size) {
    // Something else may have moved the end of the heap with sbrk since.
    if (tail && tail->free && (char *)(tail + 1) + tail->size == heap_end()) {
        char *end = (char *)(tail + 1) + size;

        if (!set_heap_end(end))
            return NULL;
        take_free(tail);
        tail->size = size;
        return tail;
    }

    uintptr_t end = (uintptr_t)heap_end();
    struct block *b = (struct block *)((end + ALIGNMENT - 1) & ~(uintptr_t)(ALIGNMENT - 1));

    if (!set_heap_end((char *)(b + 1) + size))
        return NULL;

    *b = (struct block){.size = size, .prev = tail};
    if (tail)
        tail->next = b;
    else
        head = b;
    tail = b;
    return b;
}

// The first free block big enough: any in a bigger size's bin will do.
static struct block *find_free(size_t size) {
    for (int bin = bin_of(size); bin < BINS; bin++)
        for (struct block *b = bins[bin]; b; b = links(b)->next)
            if (b->size >= size)
                return b;
    return NULL;
}

void *malloc(size_t size) {
    HEAP_LOCKED;
    if (size > SIZE_MAX - ALIGNMENT - sizeof(struct block))
        return NULL;
    // Nothing still gets a block of its own, as glibc and musl give, since
    // programs take NULL to mean memory has run out.
    size = size ? (size + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1) : ALIGNMENT;

    struct block *b = find_free(size);

    if (b) {
        take_free(b);
        split(b, size);
    } else if (!(b = extend_heap(size))) {
        return NULL;
    }
    b->free = false;
    return b + 1;
}

void free(void *ptr) {
    if (!ptr)
        return;
    HEAP_LOCKED;

    struct block *b = (struct block *)ptr - 1;

    if (b->next && b->next->free) {
        take_free(b->next);
        merge_next(b);
    }
    if (b->prev && b->prev->free) {
        b = b->prev;
        take_free(b);
        merge_next(b);
    }

    // Free space at the end of the heap goes back to the kernel.
    if (b == tail) {
        tail = b->prev;
        if (tail)
            tail->next = NULL;
        else
            head = NULL;
        if ((char *)(b + 1) + b->size == heap_end())
            set_heap_end((char *)b);
        return;
    }
    add_free(b);
}

void *calloc(size_t count, size_t size) {
    size_t total;

    if (__builtin_mul_overflow(count, size, &total))
        return NULL;

    void *ptr = malloc(total);

    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    HEAP_LOCKED;
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    if (size > SIZE_MAX - ALIGNMENT - sizeof(struct block))
        return NULL;

    struct block *b = (struct block *)ptr - 1;
    size_t rounded = (size + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);

    // Growing into a free neighbour saves a copy.
    if (b->size < rounded && b->next && b->next->free &&
        b->size + sizeof(struct block) + b->next->size >= rounded) {
        take_free(b->next);
        merge_next(b);
    }

    if (b->size >= rounded) {
        split(b, rounded);
        return ptr;
    }

    void *moved = malloc(size);

    if (moved) {
        memcpy(moved, ptr, b->size);
        free(ptr);
    }
    return moved;
}

// Takes more than needed, then gives the space before the aligned address back
// as a free block of its own.
void *aligned_alloc(size_t alignment, size_t size) {
    HEAP_LOCKED;
    if (alignment == 0 || alignment & (alignment - 1) || size > SIZE_MAX / 2 - alignment)
        return NULL;
    if (alignment <= ALIGNMENT)
        return malloc(size);

    char *raw = malloc(size + alignment + MIN_SPLIT);

    if (!raw)
        return NULL;

    char *aligned = (char *)(((uintptr_t)raw + MIN_SPLIT + alignment - 1) & ~(alignment - 1));
    struct block *front = (struct block *)raw - 1;
    struct block *b = (struct block *)aligned - 1;

    b->size = (raw + front->size) - aligned;
    b->prev = front;
    b->next = front->next;
    b->free = false;
    if (b->next)
        b->next->prev = b;
    else
        tail = b;

    front->next = b;
    front->size = (char *)b - raw;
    free(raw);
    return aligned;
}
