#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "lock.h"
#include "syscall.h"

#define PAGE_SIZE 4096
#define DEFAULT_STACK_SIZE (256 * 1024)
#define MAX_ATFORK 16
#define NS_PER_SECOND 1000000000LL
#define DESTRUCTOR_ROUNDS 4

// Whether a thread can be joined, kept in thread.state.
enum { THREAD_JOINABLE, THREAD_DETACHED, THREAD_EXITED };

static int live_threads = 1;

int sched_yield(void) {
    syscall3(SYS_YIELD, 0, 0, 0);
    return 0;
}

pthread_t pthread_self(void) {
    return __current_thread();
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

static void (*destructors[PTHREAD_KEYS_MAX])(void *);
static bool key_used[PTHREAD_KEYS_MAX];
static recursive_lock key_lock;

static void run_destructors(struct thread *self) {
    if (!self->specific)
        return;

    for (int round = 0; round < DESTRUCTOR_ROUNDS; round++) {
        bool any = false;

        for (int key = 0; key < PTHREAD_KEYS_MAX; key++) {
            void *value = self->specific[key];

            if (value && destructors[key]) {
                self->specific[key] = NULL;
                destructors[key](value);
                any = true;
            }
        }
        if (!any)
            break;
    }
    free(self->specific);
    self->specific = NULL;
}

void pthread_exit(void *result) {
    struct thread *self = __current_thread();

    while (self->cleanups) {
        struct cleanup_entry *entry = self->cleanups;

        self->cleanups = entry->next;
        entry->routine(entry->arg);
    }
    run_destructors(self);

    self->result = result;

    // The process ends with its last thread, as though it had called exit.
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL) == 0)
        exit(0);

    uint32_t state = __atomic_exchange_n(&self->state, THREAD_EXITED, __ATOMIC_ACQ_REL);
    void *unmap = state == THREAD_DETACHED ? self->map_base : NULL;
    size_t unmap_size = state == THREAD_DETACHED ? self->map_size : 0;

    syscall3(SYS_THREAD_EXIT, (long)unmap, unmap_size, 0);
    __builtin_unreachable();
}

// Where a new thread starts, on its own stack.
__attribute__((used, visibility("hidden"))) void __thread_main(void) {
    struct thread *self = __current_thread();

    self->id = syscall3(SYS_GETTID, 0, 0, 0);
    pthread_exit(self->start(self->arg));
}

// Returns the new thread's id, or a negative errno.
long __thread_clone(void *stack, void *thread_pointer, void *clear_on_exit);
__asm__(".text\n"
        ".type __thread_clone, @function\n"
        ".hidden __thread_clone\n"
        "__thread_clone:\n"
        "    movl $52, %eax\n"
        "    syscall\n"
        "    testq %rax, %rax\n"
        "    jnz 1f\n"
        "    xorl %ebp, %ebp\n"
        "    call __thread_main\n"
        "1:  ret\n");

_Static_assert(SYS_THREAD_CREATE == 52, "__thread_clone has the number written in");

int pthread_attr_init(pthread_attr_t *attr) {
    *attr = (pthread_attr_t){.stack_size = DEFAULT_STACK_SIZE, .guard_size = PAGE_SIZE};
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (size < PTHREAD_STACK_MIN)
        return EINVAL;
    attr->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size) {
    *size = attr->stack_size;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t size) {
    attr->guard_size = size;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size) {
    *size = attr->guard_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->detach_state = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state) {
    *state = attr->detach_state;
    return 0;
}

static size_t page_round(size_t size) {
    return (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
}

// From the bottom: a guard, the stack, the __thread variables, and the thread itself.
int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*start)(void *), void *arg) {
    pthread_attr_t defaults;

    if (!attr) {
        pthread_attr_init(&defaults);
        attr = &defaults;
    }

    size_t guard = page_round(attr->guard_size);
    size_t stack = page_round(attr->stack_size);
    size_t aligned_tls = (__tls_size + __tls_align - 1) & ~(__tls_align - 1);
    size_t top = page_round(aligned_tls + sizeof(struct thread) + __tls_align + 64);
    size_t total = guard + stack + top;
    char *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (base == MAP_FAILED)
        return EAGAIN;
    if (guard)
        mprotect(base, guard, PROT_NONE);

    char *stack_top = base + guard + stack;
    uintptr_t tp = ((uintptr_t)stack_top + aligned_tls + __tls_align - 1) & ~(__tls_align - 1);
    struct thread *t = (struct thread *)tp;

    if (__tls_image_size)
        memcpy((char *)tp - aligned_tls, __tls_image, __tls_image_size);

    t->self = t;
    t->exited = 1;
    t->start = start;
    t->arg = arg;
    t->state = attr->detach_state == PTHREAD_CREATE_DETACHED ? THREAD_DETACHED : THREAD_JOINABLE;
    t->map_base = base;
    t->map_size = total;

    __threaded = true;
    __atomic_add_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);

    long id = __thread_clone((void *)((uintptr_t)stack_top & ~(uintptr_t)15), t, &t->exited);

    if (id < 0) {
        __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
        munmap(base, total);
        return -id;
    }
    t->id = id;
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **result) {
    if (t == __current_thread())
        return EDEADLK;
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_DETACHED)
        return EINVAL;

    uint32_t exited;

    while ((exited = __atomic_load_n(&t->exited, __ATOMIC_ACQUIRE)) != 0)
        __futex_wait(&t->exited, exited, 0);

    if (result)
        *result = t->result;
    munmap(t->map_base, t->map_size);
    return 0;
}

int pthread_detach(pthread_t t) {
    uint32_t expected = THREAD_JOINABLE;

    if (__atomic_compare_exchange_n(&t->state, &expected, THREAD_DETACHED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
    if (expected == THREAD_DETACHED)
        return EINVAL;

    // It already exited, so nothing else will free it.
    return pthread_join(t, NULL);
}

int pthread_kill(pthread_t t, int number) {
    return -(int)syscall3(SYS_TGKILL, t->id, number, 0);
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *previous) {
    return sigprocmask(how, set, previous) < 0 ? errno : 0;
}

int pthread_once(pthread_once_t *once, void (*routine)(void)) {
    // 0 is not done, 1 is being done, and 2 is done.
    for (;;) {
        uint32_t state = __atomic_load_n(once, __ATOMIC_ACQUIRE);

        if (state == 2)
            return 0;

        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            routine();
            __atomic_store_n(once, 2, __ATOMIC_RELEASE);
            __futex_wake(once, INT32_MAX);
            return 0;
        }
        __futex_wait(once, 1, 0);
    }
}

static struct {
    void (*prepare)(void);
    void (*parent)(void);
    void (*child)(void);
} atfork_handlers[MAX_ATFORK];
static int atfork_count;

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    if (atfork_count == MAX_ATFORK)
        return ENOMEM;
    atfork_handlers[atfork_count].prepare = prepare;
    atfork_handlers[atfork_count].parent = parent;
    atfork_handlers[atfork_count].child = child;
    atfork_count++;
    return 0;
}

// Prepare handlers run newest first, and the others oldest first.
void __pthread_fork(int phase) {
    if (phase == 0) {
        for (int i = atfork_count - 1; i >= 0; i--) {
            if (atfork_handlers[i].prepare)
                atfork_handlers[i].prepare();
        }
        return;
    }
    if (phase == 2)
        live_threads = 1;
    for (int i = 0; i < atfork_count; i++) {
        void (*handler)(void) = phase == 1 ? atfork_handlers[i].parent : atfork_handlers[i].child;

        if (handler)
            handler();
    }
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    *attr = (pthread_mutexattr_t){0};
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    *type = attr->type;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared) {
    attr->shared = shared;
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *shared) {
    *shared = attr->shared;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    *mutex = (pthread_mutex_t){.type = attr ? attr->type : PTHREAD_MUTEX_NORMAL};
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    return mutex->word ? EBUSY : 0;
}

static int current_id(void) {
    return __current_thread()->id;
}

// Returns time left until a deadline by the given clock, in ns. 0 means there's none.
static int64_t time_left(const struct timespec *deadline, int clock) {
    struct timespec now;

    clock_gettime(clock, &now);
    return (deadline->tv_sec - now.tv_sec) * NS_PER_SECOND + deadline->tv_nsec - now.tv_nsec;
}

// The word is 0 if free, 1 if held, and 2 if held with someone maybe waiting.
static int take_word(uint32_t *word, const struct timespec *deadline, int clock) {
    uint32_t expected = 0;

    if (__atomic_compare_exchange_n(word, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;

    while (__atomic_exchange_n(word, 2, __ATOMIC_ACQUIRE) != 0) {
        uint64_t timeout = 0;

        if (deadline) {
            int64_t left = time_left(deadline, clock);

            if (left <= 0)
                return ETIMEDOUT;
            timeout = left;
        }
        __futex_wait(word, 2, timeout);
    }
    return 0;
}

static int lock_mutex(pthread_mutex_t *mutex, const struct timespec *deadline) {
    int self = current_id();

    if (mutex->type != PTHREAD_MUTEX_NORMAL && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == self) {
        if (mutex->type == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
        mutex->depth++;
        return 0;
    }

    int status = take_word(&mutex->word, deadline, CLOCK_REALTIME);

    if (status == 0) {
        __atomic_store_n(&mutex->owner, self, __ATOMIC_RELAXED);
        mutex->depth = 1;
    }
    return status;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return lock_mutex(mutex, NULL);
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (deadline->tv_nsec < 0 || deadline->tv_nsec >= NS_PER_SECOND)
        return EINVAL;
    return lock_mutex(mutex, deadline);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    int self = current_id();

    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == self) {
        mutex->depth++;
        return 0;
    }

    uint32_t expected = 0;

    if (!__atomic_compare_exchange_n(&mutex->word, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return EBUSY;
    __atomic_store_n(&mutex->owner, self, __ATOMIC_RELAXED);
    mutex->depth = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (mutex->type != PTHREAD_MUTEX_NORMAL && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != current_id())
        return EPERM;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && --mutex->depth > 0)
        return 0;

    __atomic_store_n(&mutex->owner, 0, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&mutex->word, 0, __ATOMIC_RELEASE) == 2)
        __futex_wake(&mutex->word, 1);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    *attr = (pthread_condattr_t){.clock = CLOCK_REALTIME};
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int clock) {
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return EINVAL;
    attr->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *attr, int *clock) {
    *clock = attr->clock;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int shared) {
    attr->shared = shared;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    *cond = (pthread_cond_t){.clock = attr ? attr->clock : CLOCK_REALTIME};
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

// A signal changes the sequence, so one sent after unlocking can't be missed: the
// sleep then fails at once as the value it expects has changed.
static int wait_cond(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    uint32_t sequence = __atomic_load_n(&cond->sequence, __ATOMIC_RELAXED);
    int depth = mutex->depth;
    int status = 0;

    mutex->depth = 1;
    int unlocked = pthread_mutex_unlock(mutex);

    if (unlocked)
        return unlocked;

    uint64_t timeout = 0;

    if (deadline) {
        int64_t left = time_left(deadline, cond->clock);

        if (left <= 0)
            status = ETIMEDOUT;
        timeout = left;
    }
    if (status == 0 && __futex_wait(&cond->sequence, sequence, timeout) == -ETIMEDOUT)
        status = ETIMEDOUT;

    // Waiters are marked as such, as others may be sleeping on it by now.
    int self = current_id();

    while (__atomic_exchange_n(&mutex->word, 2, __ATOMIC_ACQUIRE) != 0)
        __futex_wait(&mutex->word, 2, 0);
    __atomic_store_n(&mutex->owner, self, __ATOMIC_RELAXED);
    mutex->depth = depth;
    return status;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    return wait_cond(cond, mutex, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (deadline->tv_nsec < 0 || deadline->tv_nsec >= NS_PER_SECOND)
        return EINVAL;
    return wait_cond(cond, mutex, deadline);
}

int pthread_cond_signal(pthread_cond_t *cond) {
    __atomic_add_fetch(&cond->sequence, 1, __ATOMIC_RELEASE);
    __futex_wake(&cond->sequence, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    __atomic_add_fetch(&cond->sequence, 1, __ATOMIC_RELEASE);
    __futex_wake(&cond->sequence, INT32_MAX);
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr) {
    (void)attr;
    *lock = (pthread_rwlock_t){0};
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock) {
    return lock->active_readers || lock->writer ? EBUSY : 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
    pthread_mutex_lock(&lock->lock);
    // Writers go first, or a steady stream of readers could keep them out forever.
    while (lock->writer || lock->waiting_writers)
        pthread_cond_wait(&lock->readers_can_go, &lock->lock);
    lock->active_readers++;
    pthread_mutex_unlock(&lock->lock);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock) {
    int status = 0;

    pthread_mutex_lock(&lock->lock);
    if (lock->writer || lock->waiting_writers)
        status = EBUSY;
    else
        lock->active_readers++;
    pthread_mutex_unlock(&lock->lock);
    return status;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
    pthread_mutex_lock(&lock->lock);
    lock->waiting_writers++;
    while (lock->writer || lock->active_readers)
        pthread_cond_wait(&lock->writers_can_go, &lock->lock);
    lock->waiting_writers--;
    lock->writer = 1;
    pthread_mutex_unlock(&lock->lock);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
    int status = 0;

    pthread_mutex_lock(&lock->lock);
    if (lock->writer || lock->active_readers)
        status = EBUSY;
    else
        lock->writer = 1;
    pthread_mutex_unlock(&lock->lock);
    return status;
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock) {
    pthread_mutex_lock(&lock->lock);
    if (lock->writer)
        lock->writer = 0;
    else if (lock->active_readers)
        lock->active_readers--;

    if (lock->waiting_writers && !lock->active_readers)
        pthread_cond_signal(&lock->writers_can_go);
    else if (!lock->waiting_writers)
        pthread_cond_broadcast(&lock->readers_can_go);
    pthread_mutex_unlock(&lock->lock);
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *lock, int shared) {
    (void)shared;
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
    (void)lock;
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
    return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

// The holder may not be running, so spinning could waste a whole time slice.
int pthread_spin_lock(pthread_spinlock_t *lock) {
    while (pthread_spin_trylock(lock))
        sched_yield();
    return 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned count) {
    (void)attr;
    if (count == 0)
        return EINVAL;
    *barrier = (pthread_barrier_t){.count = count};
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    return barrier->waiting ? EBUSY : 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    int status = 0;

    pthread_mutex_lock(&barrier->lock);

    unsigned cycle = barrier->cycle;

    if (++barrier->waiting == barrier->count) {
        barrier->waiting = 0;
        barrier->cycle++;
        pthread_cond_broadcast(&barrier->done);
        status = PTHREAD_BARRIER_SERIAL_THREAD;
    } else {
        while (cycle == barrier->cycle)
            pthread_cond_wait(&barrier->done, &barrier->lock);
    }
    pthread_mutex_unlock(&barrier->lock);
    return status;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    LOCKED(&key_lock);

    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!key_used[i]) {
            key_used[i] = true;
            destructors[i] = destructor;
            *key = i;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    LOCKED(&key_lock);

    if (key >= PTHREAD_KEYS_MAX || !key_used[key])
        return EINVAL;
    key_used[key] = false;
    destructors[key] = NULL;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    struct thread *self = __current_thread();

    return key < PTHREAD_KEYS_MAX && self->specific ? self->specific[key] : NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    struct thread *self = __current_thread();

    if (key >= PTHREAD_KEYS_MAX || !key_used[key])
        return EINVAL;
    if (!self->specific) {
        self->specific = calloc(PTHREAD_KEYS_MAX, sizeof(void *));
        if (!self->specific)
            return ENOMEM;
    }
    self->specific[key] = (void *)value;
    return 0;
}

void __pthread_cleanup_push(struct cleanup_entry *entry, void (*routine)(void *), void *arg) {
    struct thread *self = __current_thread();

    entry->routine = routine;
    entry->arg = arg;
    entry->next = self->cleanups;
    self->cleanups = entry;
}

void __pthread_cleanup_pop(struct cleanup_entry *entry, int execute) {
    __current_thread()->cleanups = entry->next;
    if (execute)
        entry->routine(entry->arg);
}

int sem_init(sem_t *sem, int shared, unsigned value) {
    (void)shared;
    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    *sem = (sem_t){.value = value};
    return 0;
}

int sem_destroy(sem_t *sem) {
    (void)sem;
    return 0;
}

int sem_trywait(sem_t *sem) {
    uint32_t value = __atomic_load_n(&sem->value, __ATOMIC_RELAXED);

    while (value > 0) {
        if (__atomic_compare_exchange_n(&sem->value, &value, value - 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
    }
    errno = EAGAIN;
    return -1;
}

static int wait_semaphore(sem_t *sem, const struct timespec *deadline) {
    while (sem_trywait(sem) < 0) {
        uint64_t timeout = 0;

        if (deadline) {
            int64_t left = time_left(deadline, CLOCK_REALTIME);

            if (left <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            timeout = left;
        }

        __atomic_add_fetch(&sem->waiters, 1, __ATOMIC_ACQ_REL);

        int status = __futex_wait(&sem->value, 0, timeout);

        __atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_ACQ_REL);
        if (status == -EINTR) {
            errno = EINTR;
            return -1;
        }
    }
    return 0;
}

int sem_wait(sem_t *sem) {
    return wait_semaphore(sem, NULL);
}

int sem_timedwait(sem_t *sem, const struct timespec *deadline) {
    if (deadline->tv_nsec < 0 || deadline->tv_nsec >= NS_PER_SECOND) {
        errno = EINVAL;
        return -1;
    }
    return wait_semaphore(sem, deadline);
}

int sem_post(sem_t *sem) {
    __atomic_add_fetch(&sem->value, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&sem->waiters, __ATOMIC_ACQUIRE))
        __futex_wake(&sem->value, 1);
    return 0;
}

int sem_getvalue(sem_t *sem, int *value) {
    *value = __atomic_load_n(&sem->value, __ATOMIC_RELAXED);
    return 0;
}
