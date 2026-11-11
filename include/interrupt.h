#pragma once

#include <stdint.h>

// Register state saved by src/interrupt.S, lowest address first.
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

// Fills in the table all CPUs share, and loads it on the calling CPU.
void idt_init(void);
void idt_load(void);

// Defined in src/interrupt.S. Saves the callee-saved registers and stack
// pointer of the running kernel thread and resumes another.
void switch_context(uint64_t *old_rsp, uint64_t new_rsp);

// Defined in src/interrupt.S. Where a new process starts, which gives up the
// kernel lock its creator passed on, then restores an interrupt_frame.
extern char process_first_run[];

extern char syscall_entry[];
