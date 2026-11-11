#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "syscall.h"

#define MAX_HANDLERS 32

void __remove_temporaries(void);

static void (*handlers[MAX_HANDLERS])(void);
static int handler_count;

int atexit(void (*handler)(void)) {
    if (handler_count == MAX_HANDLERS)
        return -1;
    handlers[handler_count++] = handler;
    return 0;
}

static void (*quick_handlers[MAX_HANDLERS])(void);
static int quick_handler_count;

int at_quick_exit(void (*handler)(void)) {
    if (quick_handler_count == MAX_HANDLERS)
        return -1;
    quick_handlers[quick_handler_count++] = handler;
    return 0;
}

void quick_exit(int status) {
    while (quick_handler_count > 0)
        quick_handlers[--quick_handler_count]();
    _Exit(status);
}

void exit(int status) {
    while (handler_count > 0)
        handlers[--handler_count]();
    fflush(NULL);
    __remove_temporaries();
    _Exit(status);
}

void _exit(int status) {
    _Exit(status);
}

void _Exit(int status) {
    syscall3(SYS_EXIT, status, 0, 0);
    __builtin_unreachable();
}

void __assert_fail(const char *condition, const char *file, int line, const char *function) {
    dprintf(STDERR_FILENO, "%s:%d: %s: assertion failed: %s\n", file, line, function, condition);
    abort();
}

void abort(void) {
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _Exit(STATUS_ABORTED);
}
