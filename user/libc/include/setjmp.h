#pragma once

// The callee-saved registers, stack pointer and return address.
typedef long jmp_buf[8];

__attribute__((returns_twice)) int setjmp(jmp_buf env);
[[noreturn]] void longjmp(jmp_buf env, int value);

#include <signal.h>
#include <stddef.h>

// The same, also restoring the signal mask if asked to when setting it.
typedef struct {
    jmp_buf env;
    int save_mask;
    sigset_t mask;
} sigjmp_buf[1];

#define sigsetjmp(buf, save)                                                          \
    ((buf)->save_mask = (save), (save) && sigprocmask(SIG_SETMASK, NULL, &(buf)->mask), \
     setjmp((buf)->env))
[[noreturn]] void siglongjmp(sigjmp_buf buf, int value);
