#pragma once

#include <syscall_numbers.h>

static inline long syscall3(long number, long a, long b, long c) {
    long result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "memory", "rcx", "r11");
    return result;
}

static inline long syscall6(long number, long a, long b, long c, long d, long e, long f) {
    long result;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "memory", "rcx", "r11");
    return result;
}
