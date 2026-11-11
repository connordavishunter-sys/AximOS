#pragma once

#include <sys/types.h>
#include <syscall_numbers.h>

// Waits for any child to exit. *status gets its exit status, or one of the
// STATUS_ values if the kernel ended it. status may be NULL. Returns its pid, or
// -1 if there are no children.
pid_t wait(int *status);

// The same for the children pid selects: one if positive, any if -1, those in the
// caller's group if 0, or those in group -pid. options can be WNOHANG, which then
// returns 0 if none have exited.
pid_t waitpid(pid_t pid, int *status, int options);

// A status is the plain exit status or 128 plus a signal, so a program that chooses
// to exit with 129 to 159 looks like one a signal ended, as it does to a shell.
#define WIFSIGNALED(status) ((status) > 128 && (status) < 160)
#define WIFEXITED(status) (!WIFSIGNALED(status))
#define WEXITSTATUS(status) ((status) & 0xFF)
#define WTERMSIG(status) ((status) - 128)
#define WIFSTOPPED(status) 0
#define WSTOPSIG(status) 0
#define WIFCONTINUED(status) 0
#define WCOREDUMP(status) 0
#define WUNTRACED 0

struct rusage;

// Usage isn't counted, so *usage comes back zeroed.
pid_t wait3(int *status, int options, struct rusage *usage);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage);
