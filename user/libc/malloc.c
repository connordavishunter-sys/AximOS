#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lock.h"
#include "syscall.h"

#define ALIGNMENT 16
#define MIN_SPLIT (sizeof(struct block) + ALIGNMENT)

// Blocks sit back to back on the heap, linked in address order so neighbours
// can be merged. The header size keeps payloads 16-byte aligned.
struct block {
    size_t size;
    struct block *prev;
    struct block *next;
    size_t free;
};

#define HEAP_LOCKED LOCKED(&__heap_lock)

static struct block *head;
static struct block *tail;

static char *heap_end(void) {
    return (char *)syscall3(SYS_BRK, 0, 0, 0);
}

static bool set_heap_end(char *end) {
    return (char *)syscall3(SYS_BRK, (long)end, 0, 0) == end;
}

static void split(struct block *b, size_t size) {
    if (b->size < size + MIN_SPLIT)
        return;

    struct block *rest = (struct block *)((char *)(b + 1) + size);
    rest->size = b->size - size - sizeof(struct block);
    rest->prev = b;
    rest->next = b->next;
    rest->free = true;

    if (rest->next)
        rest->next->prev = rest;
    else
        tail = rest;

    b->next = rest;
    b->size = size;
}

static struct block *extend_heap(size_t size) {
    // Something else may have moved the end of the heap with sbrk since.
    if (tail && tail->free && (char *)(tail + 1) + tail->size == heap_end()) {
        char *end = (char *)(tail + 1) + size;

        if (!set_heap_end(end))
            return NULL;
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

void *malloc(size_t size) {
    HEAP_LOCKED;
    if (size == 0 || size > SIZE_MAX - ALIGNMENT - sizeof(struct block))
        return NULL;
    size = (size + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);

    struct block *b = head;
    while (b && !(b->free && b->size >= size))
        b = b->next;

    if (b)
        split(b, size);
    else
        b = extend_heap(size);

    if (!b)
        return NULL;
    b->free = false;
    return b + 1;
}

// Absorbs the block after b, which must be free.
static void merge_next(struct block *b) {
    struct block *next = b->next;

    b->size += sizeof(struct block) + next->size;
    b->next = next->next;
    if (b->next)
        b->next->prev = b;
    else
        tail = b;
}

void free(void *ptr) {
    if (!ptr)
        return;
    HEAP_LOCKED;

    struct block *b = (struct block *)ptr - 1;
    b->free = true;

    if (b->next && b->next->free)
        merge_next(b);
    if (b->prev && b->prev->free) {
        b = b->prev;
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
    }
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
        b->size + sizeof(struct block) + b->next->size >= rounded)
        merge_next(b);

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
