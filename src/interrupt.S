#include <gdt.h>

// Offsets into struct cpu and struct interrupt_frame.
#define CPU_KERNEL_STACK 8
#define CPU_USER_STACK 16
#define FRAME_R11 32
#define FRAME_RCX 96
#define FRAME_RIP 136
#define FRAME_RFLAGS 152

.text

.macro push_registers
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro pop_registers
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

// Stubs for every vector, 16 bytes apart so idt.c can index them. Vectors where
// the CPU pushes no error code get a dummy to match.
.global isr_stubs
isr_stubs:
.set vector, 0
.rept 256
    .align 16
    .if !(vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21 || vector == 29 || vector == 30)
        pushq $0
    .endif
    pushq $vector
    jmp isr_common
    .set vector, vector + 1
.endr

.global isr_syscall
isr_syscall:
    pushq $0
    pushq $0x80
    jmp isr_common

isr_common:
    cld
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    push_registers
    movq %rsp, %rdi
    call interrupt_dispatch

isr_return:
    pop_registers
    addq $16, %rsp
    testb $3, 8(%rsp)
    jz 1f
    swapgs
1:
    iretq

.global process_first_run
process_first_run:
    call kernel_unlock
    jmp isr_return

// The syscall instruction leaves the user stack in place and puts the return
// address in rcx and the flags in r11. This builds the same frame as an interrupt.
.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_STACK
    movq %gs:CPU_KERNEL_STACK, %rsp
    pushq $GDT_USER_DATA
    pushq %gs:CPU_USER_STACK
    pushq %r11
    pushq $GDT_USER_CODE
    pushq %rcx
    pushq $0
    pushq $0x80
    cld
    push_registers
    movq %rsp, %rdi
    call interrupt_dispatch

    // sysret restores rip from rcx and rflags from r11, and is much faster than
    // iret. It can't restore those two registers as well, so if exec or a signal
    // changed the frame so that they matter, go the long way. A non-canonical
    // address would fault in the kernel on Intel CPUs, so check for that too.
    movq FRAME_RIP(%rsp), %rax
    cmpq FRAME_RCX(%rsp), %rax
    jne isr_return
    shrq $47, %rax
    jnz isr_return
    movq FRAME_RFLAGS(%rsp), %rax
    cmpq FRAME_R11(%rsp), %rax
    jne isr_return

    pop_registers
    addq $16, %rsp
    popq %rcx
    addq $8, %rsp
    popq %r11
    popq %rsp
    swapgs
    sysretq

// void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
.global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
