#pragma once

#include <stdint.h>

// What libc's own data is protected with. A process with one thread never pays for
// more than a check of __threaded.
extern bool __threaded;

typedef struct {
    uint32_t word;
    int owner;
    int depth;
} recursive_lock;

void __lock_take(recursive_lock *lock);
void __lock_give(recursive_lock *lock);
bool __lock_try(recursive_lock *lock);

#define LOCK_CONCAT2(a, b) a##b
#define LOCK_CONCAT(a, b) LOCK_CONCAT2(a, b)

// Holds the lock until the end of the enclosing block.
#define LOCKED(lock)                                                                        \
    recursive_lock *LOCK_CONCAT(held_, __LINE__) __attribute__((cleanup(__lock_give_ptr))) = (lock); \
    __lock_take(LOCK_CONCAT(held_, __LINE__))

void __lock_give_ptr(recursive_lock **lock);

// Sleeps while the word holds value, for at most timeout_ns unless that's 0. Returns
// 0 or a negative errno. Wakes up to count sleepers.
int __futex_wait(volatile uint32_t *word, uint32_t value, uint64_t timeout_ns);
int __futex_wake(volatile uint32_t *word, int count);

// What each thread can find through the FS register. The first field is what the
// x86-64 thread-local storage ABI wants there.
struct thread {
    struct thread *self;
    int errno_value;
    uint32_t exited;   // not 0 until it has exited, for joining
    int id;
    uint32_t state;    // whether it's detached, and whether it has exited
    void *(*start)(void *);
    void *arg;
    void *result;
    void *map_base;
    size_t map_size;
    void **specific;
    struct cleanup_entry *cleanups;
};

static inline struct thread *__current_thread(void) {
    struct thread *self;

    __asm__ volatile("movq %%fs:0, %0" : "=r"(self));
    return self;
}

// The thread-local block the program needs, from its PT_TLS segment.
extern const void *__tls_image;
extern size_t __tls_image_size;
extern size_t __tls_size;
extern size_t __tls_align;

extern recursive_lock __heap_lock;

void __fork_prepare(void);
void __fork_parent(void);
void __fork_child(void);
