#pragma once

#include <stdint.h>
#include <time.h>

// Works between processes when in shared memory. There are no named semaphores.
typedef struct {
    uint32_t value;
    uint32_t waiters;
} sem_t;

#define SEM_VALUE_MAX 0x7FFFFFFF
#define SEM_FAILED ((sem_t *)0)

// These return 0, or -1 and set errno.
int sem_init(sem_t *sem, int shared, unsigned value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_timedwait(sem_t *sem, const struct timespec *deadline);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *value);
