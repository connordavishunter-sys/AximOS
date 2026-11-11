// Exercises threads and their synchronisation, printing "ok" or "FAIL" for each check.
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 8
#define ROUNDS 20000

static int failures;

static void check(const char *name, bool ok) {
    printf("%s %s\n", ok ? "ok" : "FAIL", name);
    failures += !ok;
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *add(void *) {
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&mutex);
        counter++;
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

static __thread long own = 5;

static void *bump_own(void *arg) {
    own += (long)arg;
    sched_yield();
    return (void *)own;
}

static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int queue[16], queued, taken_sum;

static void *consume(void *) {
    for (int got = 0; got < 100; got++) {
        pthread_mutex_lock(&mutex);
        while (queued == 0)
            pthread_cond_wait(&ready, &mutex);
        taken_sum += queue[--queued];
        pthread_cond_broadcast(&ready);
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

static pthread_barrier_t barrier;
static atomic_int arrived, early;

static void *meet(void *) {
    atomic_fetch_add(&arrived, 1);
    pthread_barrier_wait(&barrier);
    if (atomic_load(&arrived) != THREADS)
        atomic_fetch_add(&early, 1);
    return NULL;
}

static sem_t slots;
static atomic_int inside, most_inside;

static void *limited(void *) {
    sem_wait(&slots);
    int now = atomic_fetch_add(&inside, 1) + 1;
    int most = atomic_load(&most_inside);

    while (now > most && !atomic_compare_exchange_weak(&most_inside, &most, now))
        ;
    usleep(2000);
    atomic_fetch_sub(&inside, 1);
    sem_post(&slots);
    return NULL;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static atomic_int once_runs;

static void run_once(void) {
    atomic_fetch_add(&once_runs, 1);
}

static void *call_once(void *) {
    pthread_once(&once, run_once);
    return NULL;
}

static void *spin(void *) {
    volatile unsigned long n = 0;

    for (unsigned long i = 0; i < 300000000UL; i++)
        n += i;
    return NULL;
}

static double seconds(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void run_all(void *(*function)(void *), void *arg) {
    pthread_t threads[THREADS];

    for (int i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, function, arg);
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);
}

int main(int argc, char **argv) {
    int cpus = argc > 1 ? atoi(argv[1]) : 1;

    run_all(add, NULL);
    check("mutex keeps every increment", counter == (long)THREADS * ROUNDS);

    pthread_t threads[THREADS];
    bool own_values = true;

    for (long i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, bump_own, (void *)i);
    for (long i = 0; i < THREADS; i++) {
        void *result;

        pthread_join(threads[i], &result);
        own_values &= (long)result == 5 + i;
    }
    check("__thread variables are per thread", own_values && own == 5);

    pthread_t consumer;
    int put_sum = 0;

    pthread_create(&consumer, NULL, consume, NULL);
    for (int i = 1; i <= 100; i++) {
        pthread_mutex_lock(&mutex);
        while (queued == 16)
            pthread_cond_wait(&ready, &mutex);
        queue[queued++] = i;
        put_sum += i;
        pthread_cond_broadcast(&ready);
        pthread_mutex_unlock(&mutex);
    }
    pthread_join(consumer, NULL);
    check("condition variable hands over every item", taken_sum == put_sum);

    pthread_barrier_init(&barrier, NULL, THREADS);
    run_all(meet, NULL);
    check("barrier holds threads until all arrive", early == 0);

    sem_init(&slots, 0, 3);
    run_all(limited, NULL);
    check("semaphore lets three in at a time", most_inside >= 1 && most_inside <= 3);

    run_all(call_once, NULL);
    check("pthread_once runs once", once_runs == 1);

    pthread_mutex_lock(&mutex);
    check("trylock fails while locked", pthread_mutex_trylock(&mutex) == EBUSY);
    pthread_mutex_unlock(&mutex);

    // Busy threads on separate CPUs take about as long as one does alone.
    double start = seconds();
    spin(NULL);
    double alone = seconds() - start;
    pthread_t spinners[4];
    int n = cpus < 4 ? cpus : 4;

    start = seconds();
    for (int i = 0; i < n; i++)
        pthread_create(&spinners[i], NULL, spin, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(spinners[i], NULL);
    double together = seconds() - start;
    printf("one spinner %.2fs, %d at once %.2fs\n", alone, n, together);
    check("threads run in parallel", together < alone * 1.6);

    // A threaded process can still fork and exec.
    pid_t child = fork();

    if (child == 0) {
        execl("/system/bin/echo", "echo", "exec after threads", NULL);
        _exit(1);
    }

    int status;

    check("fork and exec from a threaded process", waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("threads: %d failed\n", failures);
    return failures != 0;
}
