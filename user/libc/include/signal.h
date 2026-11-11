#pragma once

#include <signal_numbers.h>
#include <sys/types.h>

typedef int sig_atomic_t;

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)SIG_DFL_VALUE)
#define SIG_IGN ((sighandler_t)SIG_IGN_VALUE)
#define SIG_ERR ((sighandler_t)-1)

// Sets what a signal does, returning what it did before or SIG_ERR. A signal stays
// blocked while its own handler runs, and system calls it interrupts fail with EINTR.
sighandler_t signal(int number, sighandler_t handler);

// sa_restorer is filled in. Stopping and continuing signals have no default effect,
// as there's no job control.
int sigaction(int number, const struct sigaction *action, struct sigaction *previous);

int sigprocmask(int how, const sigset_t *set, sigset_t *previous);
int sigpending(sigset_t *set);

// Waits for a handler to run with the given mask in place. Always returns -1 with
// errno set to EINTR.
int sigsuspend(const sigset_t *mask);

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int number);
int sigdelset(sigset_t *set, int number);
int sigismember(const sigset_t *set, int number);

// Sends a signal. pid can name a process, a process group as a negative number,
// or with 0 the caller's own group.
int kill(pid_t pid, int number);

int raise(int number);

// Describes a signal, like "Interrupt".
char *strsignal(int number);
void psignal(int number, const char *s);
