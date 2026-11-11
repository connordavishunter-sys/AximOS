#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

#define SYSCALL_STRING_VALUE(value) #value
#define SYSCALL_STRING(value) SYSCALL_STRING_VALUE(value)

static long checked(long result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

// Handlers return here, which asks the kernel to resume what they interrupted.
__asm__(".text\n"
        "signal_restorer:\n"
        "    movl $" SYSCALL_STRING(SYS_SIGRETURN) ", %eax\n"
        "    syscall\n");
extern char signal_restorer[];

int sigaction(int number, const struct sigaction *action, struct sigaction *previous) {
    struct sigaction copy;

    if (action) {
        copy = *action;
        copy.sa_restorer = (void (*)(void))signal_restorer;
    }
    return checked(syscall3(SYS_SIGACTION, number, action ? (long)&copy : 0, (long)previous));
}

sighandler_t signal(int number, sighandler_t handler) {
    struct sigaction action = {.sa_handler = handler}, previous;

    return sigaction(number, &action, &previous) < 0 ? SIG_ERR : previous.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *previous) {
    return checked(syscall3(SYS_SIGPROCMASK, how, (long)set, (long)previous));
}

int sigpending(sigset_t *set) {
    return checked(syscall3(SYS_SIGPENDING, (long)set, 0, 0));
}

int sigsuspend(const sigset_t *mask) {
    return checked(syscall3(SYS_SIGSUSPEND, *mask, 0, 0));
}

static bool valid(int number) {
    if (number > 0 && number < NSIG)
        return true;
    errno = EINVAL;
    return false;
}

int sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    *set = ~(sigset_t)1;
    return 0;
}

int sigaddset(sigset_t *set, int number) {
    if (!valid(number))
        return -1;
    *set |= 1u << number;
    return 0;
}

int sigdelset(sigset_t *set, int number) {
    if (!valid(number))
        return -1;
    *set &= ~(1u << number);
    return 0;
}

int sigismember(const sigset_t *set, int number) {
    return valid(number) ? (int)(*set >> number & 1) : -1;
}

int kill(pid_t pid, int number) {
    return checked(syscall3(SYS_KILL, pid, number, 0));
}

int raise(int number) {
    return kill(getpid(), number);
}

unsigned alarm(unsigned seconds) {
    return syscall3(SYS_ALARM, seconds, 0, 0);
}

int pause(void) {
    sigset_t mask;

    sigprocmask(SIG_SETMASK, NULL, &mask);
    return sigsuspend(&mask);
}

void siglongjmp(sigjmp_buf buf, int value) {
    if (buf->save_mask)
        sigprocmask(SIG_SETMASK, &buf->mask, NULL);
    longjmp(buf->env, value);
}

char *strsignal(int number) {
    switch (number) {
    case SIGHUP: return "Hangup";
    case SIGINT: return "Interrupt";
    case SIGQUIT: return "Quit";
    case SIGILL: return "Illegal instruction";
    case SIGTRAP: return "Trace/breakpoint trap";
    case SIGABRT: return "Aborted";
    case SIGBUS: return "Bus error";
    case SIGFPE: return "Floating point exception";
    case SIGKILL: return "Killed";
    case SIGUSR1: return "User defined signal 1";
    case SIGSEGV: return "Segmentation fault";
    case SIGUSR2: return "User defined signal 2";
    case SIGPIPE: return "Broken pipe";
    case SIGALRM: return "Alarm clock";
    case SIGTERM: return "Terminated";
    case SIGCHLD: return "Child exited";
    case SIGCONT: return "Continued";
    case SIGSTOP: return "Stopped (signal)";
    case SIGTSTP: return "Stopped";
    case SIGTTIN: return "Stopped (tty input)";
    case SIGTTOU: return "Stopped (tty output)";
    case SIGURG: return "Urgent I/O condition";
    case SIGWINCH: return "Window changed";
    default: return "Unknown signal";
    }
}

void psignal(int number, const char *s) {
    if (s && *s)
        dprintf(STDERR_FILENO, "%s: ", s);
    dprintf(STDERR_FILENO, "%s\n", strsignal(number));
}
