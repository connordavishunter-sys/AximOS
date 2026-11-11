#include <errno.h>
#include <stddef.h>
#include "lock.h"
#include "syscall.h"

recursive_lock __heap_lock;

int *__errno_location(void) {
    return &__current_thread()->errno_value;
}

int __futex_wait(volatile uint32_t *word, uint32_t value, uint64_t timeout_ns) {
    return syscall6(SYS_FUTEX, (long)word, FUTEX_WAIT, value, timeout_ns, 0, 0);
}

int __futex_wake(volatile uint32_t *word, int count) {
    return syscall6(SYS_FUTEX, (long)word, FUTEX_WAKE, count, 0, 0, 0);
}

bool __lock_try(recursive_lock *lock) {
    if (!__threaded)
        return true;

    int self = __current_thread()->id;

    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == self) {
        lock->depth++;
        return true;
    }

    uint32_t expected = 0;

    if (!__atomic_compare_exchange_n(&lock->word, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return false;
    __atomic_store_n(&lock->owner, self, __ATOMIC_RELAXED);
    lock->depth = 1;
    return true;
}

void __lock_take(recursive_lock *lock) {
    if (!__threaded)
        return;

    int self = __current_thread()->id;

    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == self) {
        lock->depth++;
        return;
    }

    // 0 is free, 1 is held, and 2 is held with someone maybe waiting.
    uint32_t expected = 0;

    if (!__atomic_compare_exchange_n(&lock->word, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        while (__atomic_exchange_n(&lock->word, 2, __ATOMIC_ACQUIRE) != 0)
            __futex_wait(&lock->word, 2, 0);
    }
    __atomic_store_n(&lock->owner, self, __ATOMIC_RELAXED);
    lock->depth = 1;
}

void __lock_give(recursive_lock *lock) {
    // Threads may have started since this was taken, so it may not be held at all.
    if (!__threaded || lock->depth == 0 || --lock->depth > 0)
        return;

    __atomic_store_n(&lock->owner, 0, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&lock->word, 0, __ATOMIC_RELEASE) == 2)
        __futex_wake(&lock->word, 1);
}

void __lock_give_ptr(recursive_lock **lock) {
    __lock_give(*lock);
}

// These are only there if the program uses what they belong to.
__attribute__((weak)) void __stdio_fork_prepare(void);
__attribute__((weak)) void __stdio_fork_parent(void);
__attribute__((weak)) void __stdio_fork_child(void);
__attribute__((weak)) void __pthread_fork(int phase);

// The other threads don't exist in the child, so nothing they held can be left
// half done: everything is taken before the fork and let go on both sides of it.
void __fork_prepare(void) {
    if (__pthread_fork)
        __pthread_fork(0);
    if (!__threaded)
        return;
    __lock_take(&__heap_lock);
    if (__stdio_fork_prepare)
        __stdio_fork_prepare();
}

void __fork_parent(void) {
    if (__threaded) {
        if (__stdio_fork_parent)
            __stdio_fork_parent();
        __lock_give(&__heap_lock);
    }
    if (__pthread_fork)
        __pthread_fork(1);
}

void __fork_child(void) {
    struct thread *self = __current_thread();

    self->id = syscall3(SYS_GETTID, 0, 0, 0);
    if (__threaded) {
        __heap_lock.owner = self->id;
        if (__stdio_fork_child)
            __stdio_fork_child();
        __lock_give(&__heap_lock);
    }
    if (__pthread_fork)
        __pthread_fork(2);
}
