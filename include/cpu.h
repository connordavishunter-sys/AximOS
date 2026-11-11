#pragma once

#include <stdint.h>

struct process;

// One per CPU, found through the GS base. src/interrupt.S knows the offsets of
// the first three fields.
struct cpu {
    struct cpu *self;
    uint64_t kernel_stack;
    uint64_t user_stack; // scratch space while entering through syscall
    struct process *running;
    uint64_t idle_rsp;
    int index;
    uint32_t apic_id;
    bool ready; // has an interrupt table, so it can take an NMI
    uint64_t address_space; // changes with the kernel lock held
    bool flush_tlb; // set by another CPU that changed that address space
    bool idle;      // halted, or on the way to it, with nothing to do
    uint64_t idle_ticks; // timer interrupts that found it with nothing to do
};

static inline struct cpu *this_cpu(void) {
    struct cpu *cpu;

    __asm__ volatile("movq %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

// Sets up the CPU this runs on, which must be the boot CPU, except for the parts
// that depend on the memory manager.
void cpu_init_boot(void);

// Starts the other CPUs. Each waits for the kernel lock, then goes idle.
void cpu_start_others(void);

int cpu_count(void);

// Timer interrupts that found one CPU idle, for telling how busy it's been.
uint64_t cpu_idle_ticks(int index);

// Sets the stack used when entering the kernel from ring 3 on this CPU.
void cpu_set_kernel_stack(uint64_t top);

void cpu_set_thread_pointer(uint64_t value);

// Gets a halted CPU looking for something to run, if there's one to ask.
void cpu_wake_one_idle(void);

// The big kernel lock. Only one CPU runs kernel code at a time, with interrupts
// off, so nothing else in the kernel has to think about concurrency. It is held
// across context switches and released by whichever thread runs next.
void kernel_lock(void);
void kernel_unlock(void);

// After changing the active address space's page tables, makes every other CPU using
// it forget the old translations, and waits for them to. Needs the kernel lock.
void cpu_flush_others(void);

// Does what cpu_flush_others asked of this CPU.
void cpu_handle_flush(void);

// Stops every other CPU, which is only for when the kernel is dying.
void cpu_stop_others(void);
bool cpu_stopping(void);
