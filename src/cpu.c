#include <acpi.h>
#include <cpu.h>
#include <gdt.h>
#include <interrupt.h>
#include <irq.h>
#include <limine.h>
#include <mm.h>
#include <process.h>
#include <serial.h>
#include <stdatomic.h>
#include <stddef.h>

#define MAX_CPUS 64
#define DOUBLE_FAULT_STACK_SIZE 8192

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define EFER_SYSCALL 0x1

#define CR0_MONITOR (1ULL << 1)
#define CR0_EMULATION (1ULL << 2)
#define CR0_TASK_SWITCHED (1ULL << 3)
#define CR0_NATIVE_ERRORS (1ULL << 5)
#define CR4_FXSAVE (1ULL << 9)
#define CR4_SIMD_EXCEPTIONS (1ULL << 10)

// What syscall clears in RFLAGS: interrupts, direction, trap and alignment check.
#define SYSCALL_FLAG_MASK 0x40700

// syscall loads CS and SS from the first selector here. sysret loads SS from the
// second plus 8 and CS from it plus 16, which is why user data comes before user
// code in the GDT.
#define STAR_VALUE ((uint64_t)GDT_KERNEL_DATA << 48 | (uint64_t)GDT_KERNEL_CODE << 32)

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
};

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct table_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define MSR_PAT 0x277
#define PAT_ENTRY_1_MASK 0xFF00ULL
#define PAT_ENTRY_1_WRITE_COMBINING 0x0100ULL

static struct cpu cpus[MAX_CPUS];
static struct tss tss_for[MAX_CPUS];
static uint64_t gdt_for[MAX_CPUS][7];

// A double fault can mean the kernel stack overflowed, so it gets its own.
static uint8_t double_fault_stacks[MAX_CPUS][DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

static int started_cpus = 1;
static atomic_uint lock_next_ticket;
static atomic_uint lock_now_serving;
static atomic_bool stopping;

static void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (uint64_t)high << 32 | low;
}

static void load_gdt(int index) {
    uint64_t *gdt = gdt_for[index];
    struct tss *tss = &tss_for[index];
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof(*tss) - 1;

    tss->ist[GDT_IST_DOUBLE_FAULT - 1] = (uint64_t)double_fault_stacks[index] + DOUBLE_FAULT_STACK_SIZE;
    tss->iomap_base = sizeof(*tss); // no I/O permission bitmap, so no ports in ring 3

    gdt[0] = 0;
    gdt[GDT_KERNEL_CODE / 8] = 0x00AF9A000000FFFF;
    gdt[GDT_KERNEL_DATA / 8] = 0x00CF92000000FFFF;
    gdt[GDT_USER_DATA / 8] = 0x00CFF2000000FFFF;
    gdt[GDT_USER_CODE / 8] = 0x00AFFA000000FFFF;
    gdt[GDT_TSS / 8] = (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 | 0x89ULL << 40
        | (limit >> 16 & 0xF) << 48 | (base >> 24 & 0xFF) << 56;
    gdt[GDT_TSS / 8 + 1] = base >> 32;

    struct table_pointer pointer = {sizeof(gdt_for[0]) - 1, (uint64_t)gdt};

    __asm__ volatile(
        "lgdt %[pointer]\n"
        "pushq %[code]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %[data], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %[tss], %%ax\n"
        "ltr %%ax\n"
        :
        : [pointer] "m"(pointer), [code] "i"(GDT_KERNEL_CODE), [data] "i"(GDT_KERNEL_DATA),
          [tss] "i"(GDT_TSS)
        : "rax", "memory");
}

static void enable_floating_point(void) {
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(uint64_t)(CR0_EMULATION | CR0_TASK_SWITCHED)) | CR0_MONITOR | CR0_NATIVE_ERRORS;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_FXSAVE | CR4_SIMD_EXCEPTIONS;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("fninit");
}

// Everything each CPU needs before it can take interrupts or run processes.
static void init_this_cpu(int index) {
    struct cpu *cpu = &cpus[index];

    cpu->self = cpu;
    cpu->index = index;

    load_gdt(index);

    // Makes the write-through page attribute mean write-combining instead. Nothing
    // else uses write-through alone, and every CPU has to agree on this.
    write_msr(MSR_PAT, (read_msr(MSR_PAT) & ~PAT_ENTRY_1_MASK) | PAT_ENTRY_1_WRITE_COMBINING);
    idt_load();
    enable_floating_point();

    // Loading the GS selector above cleared the base, so this has to come after.
    write_msr(MSR_GS_BASE, (uint64_t)cpu);
    write_msr(MSR_KERNEL_GS_BASE, 0);

    write_msr(MSR_EFER, read_msr(MSR_EFER) | EFER_SYSCALL);
    write_msr(MSR_STAR, STAR_VALUE);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_FMASK, SYSCALL_FLAG_MASK);
}

// Only a CPU with an interrupt table can take the stop interrupt. One still parked
// in the bootloader would triple fault, which resets the whole machine.
static void mark_ready(int index, uint32_t apic_id) {
    cpus[index].apic_id = apic_id;
    __atomic_store_n(&cpus[index].ready, true, __ATOMIC_SEQ_CST);
}

void cpu_init_boot(void) {
    kernel_lock();
    idt_init();
    init_this_cpu(0);
    cpus[0].ready = false; // nothing ever needs to stop this one
}

static void other_cpu_main(struct limine_mp_info *info) {
    init_this_cpu(info->extra_argument);
    mark_ready(info->extra_argument, info->lapic_id);
    kernel_lock();
    irq_init_this_cpu();
    process_idle_loop();
}

void cpu_start_others(void) {
    struct limine_mp_response *response = mp_request.response;

    if (!response) {
        kprintf("cpu: bootloader reported no other CPUs\n");
        return;
    }

    int index = 1;

    for (uint64_t i = 0; i < response->cpu_count && index < MAX_CPUS; i++) {
        struct limine_mp_info *info = response->cpus[i];

        if (info->lapic_id == response->bsp_lapic_id)
            continue;

        info->extra_argument = index++;
        __atomic_store_n(&info->goto_address, other_cpu_main, __ATOMIC_SEQ_CST);
    }

    started_cpus = index;
    kprintf("cpu: %u CPUs\n", started_cpus);
}

uint64_t cpu_idle_ticks(int index) {
    return cpus[index].idle_ticks;
}

int cpu_count(void) {
    return started_cpus;
}

void cpu_set_thread_pointer(uint64_t value) {
    write_msr(MSR_FS_BASE, value);
}

void cpu_set_kernel_stack(uint64_t top) {
    struct cpu *cpu = this_cpu();

    cpu->kernel_stack = top;
    tss_for[cpu->index].rsp[0] = top;
}

// Whoever waits longest goes next. Waiting has interrupts off, so a flush request has
// to be noticed here, or its sender, which holds the lock, would wait forever.
void kernel_lock(void) {
    unsigned ticket = atomic_fetch_add_explicit(&lock_next_ticket, 1, memory_order_relaxed);

    while (atomic_load_explicit(&lock_now_serving, memory_order_acquire) != ticket) {
        if (__atomic_load_n(&this_cpu()->flush_tlb, __ATOMIC_ACQUIRE))
            cpu_handle_flush();
        __asm__ volatile("pause");
    }
}

void kernel_unlock(void) {
    atomic_fetch_add_explicit(&lock_now_serving, 1, memory_order_release);
}

void cpu_handle_flush(void) {
    uint64_t cr3;

    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3));
    __atomic_store_n(&this_cpu()->flush_tlb, false, __ATOMIC_RELEASE);
}

void cpu_flush_others(void) {
    struct cpu *self = this_cpu();
    bool sent = false;

    for (int i = 0; i < started_cpus; i++) {
        if (&cpus[i] != self && cpus[i].address_space == self->address_space) {
            __atomic_store_n(&cpus[i].flush_tlb, true, __ATOMIC_RELEASE);
            irq_send_flush(cpus[i].apic_id);
            sent = true;
        }
    }
    for (int i = 0; sent && i < started_cpus; i++) {
        while (__atomic_load_n(&cpus[i].flush_tlb, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }
}

void cpu_wake_one_idle(void) {
    // An interrupt on an idle CPU goes back to looking for work, so it can take this.
    if (!this_cpu()->running)
        return;

    for (int i = 0; i < started_cpus; i++) {
        if (cpus[i].idle) {
            cpus[i].idle = false;
            irq_send_wake(cpus[i].apic_id);
            return;
        }
    }
}

void cpu_stop_others(void) {
    atomic_store(&stopping, true);

    for (int i = 0; i < MAX_CPUS; i++) {
        if (i != this_cpu()->index && __atomic_load_n(&cpus[i].ready, __ATOMIC_SEQ_CST))
            irq_send_nmi(cpus[i].apic_id);
    }
}

bool cpu_stopping(void) {
    return atomic_load(&stopping);
}
