#include <cpu.h>
#include <gdt.h>
#include <interrupt.h>
#include <irq.h>
#include <kernel.h>
#include <mm.h>
#include <net.h>
#include <process.h>
#include <serial.h>
#include <syscall.h>

#define VECTOR_NMI 2
#define VECTOR_DOUBLE_FAULT 8
#define VECTOR_PAGE_FAULT 14
#define VECTOR_EXCEPTION_COUNT 32
#define VECTOR_SYSCALL 0x80
#define PAGE_FAULT_WRITE 0x02
#define STUB_SPACING 16

#define GATE_KERNEL 0x8E
#define GATE_USER 0xEE

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern char isr_stubs[];
extern char isr_syscall[];

static struct idt_entry idt[256];

static void set_gate(int vector, void *handler, uint8_t flags, uint8_t ist) {
    uint64_t addr = (uint64_t)handler;

    idt[vector] = (struct idt_entry){
        .offset_low = addr,
        .selector = GDT_KERNEL_CODE,
        .ist = ist,
        .flags = flags,
        .offset_mid = addr >> 16,
        .offset_high = addr >> 32,
    };
}

void idt_init(void) {
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_stubs + i * STUB_SPACING, GATE_KERNEL,
                 i == VECTOR_DOUBLE_FAULT ? GDT_IST_DOUBLE_FAULT : 0);
    set_gate(VECTOR_SYSCALL, isr_syscall, GATE_USER, 0);
}

void idt_load(void) {
    struct idt_pointer pointer = {sizeof(idt) - 1, (uint64_t)idt};

    __asm__ volatile("lidt %0" : : "m"(pointer));
}

// A CPU in the kernel holds the lock, so an exception there can't wait for it.
[[noreturn]] static void kernel_exception(const struct interrupt_frame *frame, uint64_t cr2) {
    if (frame->vector == VECTOR_DOUBLE_FAULT)
        kprintf("exception: double fault, the kernel stack may have overflowed\n");

    kprintf("exception: vector %lu error %lx rip %lx rsp %lx cr2 %lx\n",
            frame->vector, frame->error, frame->rip, frame->rsp, cr2);
    die("exception in kernel");
}

static void user_exception(struct interrupt_frame *frame, uint64_t cr2) {
    // A write to a page shared by fork isn't really a fault.
    if (frame->vector == VECTOR_PAGE_FAULT && (frame->error & PAGE_FAULT_WRITE)
        && mm_handle_write_fault(cr2))
        return;

    kprintf("exception: vector %lu error %lx rip %lx rsp %lx cr2 %lx\n",
            frame->vector, frame->error, frame->rip, frame->rsp, cr2);
    process_fault(frame->vector);
}

void interrupt_dispatch(struct interrupt_frame *frame) {
    // Read this first, since a successful exec replaces the whole frame.
    uint64_t vector = frame->vector;

    if (vector == VECTOR_NMI) {
        if (cpu_stopping())
            for (;;)
                __asm__ volatile("cli; hlt");
        return;
    }

    // Needs no lock: the CPU asking holds it and is waiting for this.
    if (vector == VECTOR_FLUSH) {
        cpu_handle_flush();
        irq_acknowledge();
        return;
    }

    bool from_user = (frame->cs & 3) == 3;
    uint64_t cr2 = 0;

    if (vector == VECTOR_PAGE_FAULT)
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    // The kernel lock is already held, as the kernel was running.
    if (vector == VECTOR_PAGE_FAULT && !from_user && this_cpu()->running && cr2 >= PAGE_SIZE && cr2 < LOWER_HALF_END) {
        process_handle_kernel_fault(cr2, frame->error & PAGE_FAULT_WRITE);
        return;
    }
    if (vector < VECTOR_EXCEPTION_COUNT && !from_user)
        kernel_exception(frame, cr2);

    kernel_lock();

    if (vector == VECTOR_SYSCALL)
        syscall_dispatch(frame);
    else if (vector < VECTOR_EXCEPTION_COUNT)
        user_exception(frame, cr2);
    else
        irq_handle(frame);

    net_flush();
    if (from_user)
        process_deliver_signals(frame);

    kernel_unlock();
}
