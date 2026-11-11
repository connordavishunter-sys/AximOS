#pragma once

#include <stdint.h>

// Shared by the kernel and libc. The numbers match Linux. A process ended by a
// signal exits with 128 plus its number.

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31
#define NSIG 32

#define SIG_DFL_VALUE 0
#define SIG_IGN_VALUE 1

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// sa_flags. There's no siginfo, so handlers always take just the signal number.
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_NOCLDSTOP 0x00000001

// Signal n is bit n.
typedef uint32_t sigset_t;

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    uint32_t sa_flags;
    void (*sa_restorer)(void);
};
