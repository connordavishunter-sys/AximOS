#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

// Threads of a process run at the same time, on as many CPUs as there are.

typedef struct thread *pthread_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    size_t stack_size;
    size_t guard_size;
    int detach_state;
} pthread_attr_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

// All of these work between processes when in shared memory.
typedef struct {
    int type;
    int shared;
} pthread_mutexattr_t;

typedef struct {
    uint32_t word;
    int type;
    int owner;
    int depth;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {.type = PTHREAD_MUTEX_RECURSIVE}

typedef struct {
    int clock;
    int shared;
} pthread_condattr_t;

typedef struct {
    uint32_t sequence;
    int clock;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0}

typedef struct {
    int shared;
} pthread_rwlockattr_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t readers_can_go;
    pthread_cond_t writers_can_go;
    int active_readers;
    int waiting_writers;
    int writer;
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER {0}

typedef uint32_t pthread_once_t;
#define PTHREAD_ONCE_INIT 0

typedef uint32_t pthread_spinlock_t;

typedef unsigned pthread_key_t;
#define PTHREAD_KEYS_MAX 128
#define PTHREAD_STACK_MIN 16384

typedef struct {
    int shared;
} pthread_barrierattr_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done;
    unsigned count;
    unsigned waiting;
    unsigned cycle;
} pthread_barrier_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

// Everything below returns 0 or an errno value, rather than setting errno, except
// where a comment says otherwise.
int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
[[noreturn]] void pthread_exit(void *result);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_kill(pthread_t thread, int number);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *previous);
int pthread_once(pthread_once_t *once, void (*routine)(void));

// Each runs in its side of a fork: prepare before, and the others after.
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *shared);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setclock(pthread_condattr_t *attr, int clock);
int pthread_condattr_getclock(const pthread_condattr_t *attr, int *clock);
int pthread_condattr_setpshared(pthread_condattr_t *attr, int shared);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *lock);
int pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock);
int pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *lock);
int pthread_rwlock_unlock(pthread_rwlock_t *lock);

int pthread_spin_init(pthread_spinlock_t *lock, int shared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

struct cleanup_entry {
    void (*routine)(void *);
    void *arg;
    struct cleanup_entry *next;
};

void __pthread_cleanup_push(struct cleanup_entry *entry, void (*routine)(void *), void *arg);
void __pthread_cleanup_pop(struct cleanup_entry *entry, int execute);

// These open and close a block, so they must be used in pairs in one function.
#define pthread_cleanup_push(routine, arg) \
    {                                      \
        struct cleanup_entry __cleanup;    \
        __pthread_cleanup_push(&__cleanup, routine, arg);
#define pthread_cleanup_pop(execute)                  \
        __pthread_cleanup_pop(&__cleanup, execute);   \
    }
