#include <acpi.h>
#include <clock.h>
#include <cpu.h>
#include <io.h>
#include <irq.h>
#include <kmalloc.h>
#include <limine.h>
#include <mm.h>
#include <pci.h>
#include <process.h>
#include <serial.h>
#include <uacpi/kernel_api.h>

// What uACPI needs from the kernel. The kernel lock already keeps everything else
// out, so its spinlocks only have to keep interrupts out, and its semaphores are
// counts that are slept on. Its deferred work runs on a kernel thread of its own.

#define FOREVER 0xFFFF
#define RFLAGS_INTERRUPTS 0x200

static char boot_thread; // who's running before there are threads

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    uint64_t rsdp = acpi_rsdp_address();

    if (!rsdp)
        return UACPI_STATUS_NOT_FOUND;
    *out = rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    void *virt = mm_map_physical(addr, len);

    return virt ? virt : UACPI_MAP_FAILED;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr, (void)len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *message) {
    kprintf("acpi: %s%s", level <= UACPI_LOG_WARN ? "warning: " : "", message);
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out) {
    struct pci_device *d = kmalloc(sizeof(*d));

    if (address.segment != 0 || !d) {
        kfree(d);
        return UACPI_STATUS_UNIMPLEMENTED;
    }
    *d = (struct pci_device){address.bus, address.device, address.function};
    *out = d;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle device) {
    kfree(device);
}

// Configuration space is read and written 32 bits at a time.
static uint32_t pci_get(uacpi_handle device, uacpi_size offset, int bytes) {
    uint32_t word = pci_read(*(struct pci_device *)device, offset & ~3);

    return bytes == 4 ? word : word >> (offset & 3) * 8 & ((1u << bytes * 8) - 1);
}

static void pci_put(uacpi_handle device, uacpi_size offset, int bytes, uint32_t value) {
    struct pci_device *d = device;

    if (bytes != 4) {
        uint32_t shift = (offset & 3) * 8, mask = ((1u << bytes * 8) - 1) << shift;

        value = (pci_read(*d, offset & ~3) & ~mask) | (value << shift & mask);
    }
    pci_write(*d, offset & ~3, value);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle d, uacpi_size offset, uacpi_u8 *value) {
    *value = pci_get(d, offset, 1);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle d, uacpi_size offset, uacpi_u16 *value) {
    *value = pci_get(d, offset, 2);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle d, uacpi_size offset, uacpi_u32 *value) {
    *value = pci_get(d, offset, 4);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle d, uacpi_size offset, uacpi_u8 value) {
    pci_put(d, offset, 1, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle d, uacpi_size offset, uacpi_u16 value) {
    pci_put(d, offset, 2, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle d, uacpi_size offset, uacpi_u32 value) {
    pci_put(d, offset, 4, value);
    return UACPI_STATUS_OK;
}

// An I/O range is just its first port.
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    (void)len;
    *out = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    (void)handle;
}

#define PORT(handle, offset) ((uint16_t)((uintptr_t)(handle) + (offset)))

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size offset, uacpi_u8 *out) {
    *out = inb(PORT(h, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size offset, uacpi_u16 *out) {
    *out = inw(PORT(h, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size offset, uacpi_u32 *out) {
    *out = inl(PORT(h, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size offset, uacpi_u8 value) {
    outb(PORT(h, offset), value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size offset, uacpi_u16 value) {
    outw(PORT(h, offset), value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size offset, uacpi_u32 value) {
    outl(PORT(h, offset), value);
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_alloc(uacpi_size size) {
    return kmalloc(size);
}

void uacpi_kernel_free(void *mem) {
    kfree(mem);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    return clock_monotonic_ns();
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    for (uint64_t until = clock_monotonic_ns() + usec * 1000ULL; clock_monotonic_ns() < until;)
        ;
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    uint64_t until = clock_monotonic_ns() + msec * 1000000;
    char channel;

    while (clock_monotonic_ns() < until) {
        if (process_can_sleep())
            process_sleep_uninterruptible(&channel, (until - clock_monotonic_ns()) * TIMER_HZ / NS_PER_SECOND + 1);
    }
}

struct semaphore {
    uint64_t units;
};

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial_units) {
    struct semaphore *s = kmalloc(sizeof(*s));

    if (s)
        s->units = initial_units;
    return s;
}

void uacpi_kernel_free_semaphore(uacpi_handle s) {
    kfree(s);
}

// Before there are threads, nothing else could give a unit, so only a wait with a
// timeout can end, by running out.
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle handle, uacpi_u16 timeout) {
    struct semaphore *s = handle;
    uint64_t until = clock_monotonic_ns() + timeout * 1000000ULL;

    while (!s->units) {
        uint64_t now = clock_monotonic_ns();

        if (timeout != FOREVER && now >= until)
            return UACPI_STATUS_TIMEOUT;
        if (process_can_sleep())
            process_sleep_uninterruptible(s, timeout == FOREVER ? 0 : (until - now) * TIMER_HZ / NS_PER_SECOND + 1);
        else if (timeout == FOREVER)
            return UACPI_STATUS_INTERNAL_ERROR;
    }
    s->units--;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_signal_semaphore(uacpi_handle handle) {
    struct semaphore *s = handle;

    s->units++;
    process_wakeup(s);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    struct process *p = this_cpu()->running;

    return p ? (uacpi_thread_id)p : &boot_thread;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
    return flags;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    if (state & RFLAGS_INTERRUPTS)
        __asm__ volatile("sti");
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    return &boot_thread; // never looked at
}

void uacpi_kernel_free_spinlock(uacpi_handle lock) {
    (void)lock;
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle lock) {
    (void)lock;
    return uacpi_kernel_disable_interrupts();
}

void uacpi_kernel_unlock_spinlock(uacpi_handle lock, uacpi_cpu_flags flags) {
    (void)lock;
    uacpi_kernel_restore_interrupts(flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        kprintf("acpi: the firmware reports a fatal error %x, code %x\n", request->fatal.type, request->fatal.code);
    return UACPI_STATUS_OK;
}

// The SCI, the one interrupt uACPI asks for.
static uacpi_interrupt_handler irq_handler;
static uacpi_handle irq_context;
static int irq_number = -1;

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context,
                                                    uacpi_handle *out) {
    if (irq >= 16 || irq_handler)
        return UACPI_STATUS_UNIMPLEMENTED;
    irq_handler = handler;
    irq_context = context;
    irq_number = irq;
    acpi_route_sci(irq);
    irq_enable_isa(irq);
    *out = &irq_number;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle irq) {
    (void)handler, (void)irq;
    irq_handler = NULL;
    irq_number = -1;
    return UACPI_STATUS_OK;
}

bool acpi_handle_irq(int irq) {
    return irq == irq_number && irq_handler && irq_handler(irq_context) == UACPI_INTERRUPT_HANDLED;
}

// Deferred work, in the order it was asked for.
struct work {
    uacpi_work_handler handler;
    uacpi_handle context;
    struct work *next;
};

static struct work *first_work, *last_work;
static bool working;
static bool have_worker;

uacpi_handle uacpi_kernel_create_work_item(void) {
    return kmalloc(sizeof(struct work));
}

void uacpi_kernel_free_work_item(uacpi_handle item) {
    kfree(item);
}

void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle item, uacpi_work_handler handler, uacpi_handle context) {
    struct work *w = item;

    (void)type;
    *w = (struct work){handler, context, NULL};
    if (last_work)
        last_work->next = w;
    else
        first_work = w;
    last_work = w;
    process_wakeup(&first_work);
}

// The item may be scheduled again or freed by its handler, so it's left alone after.
static bool run_next_work(void) {
    struct work *w = first_work;

    if (!w)
        return false;
    first_work = w->next;
    if (!first_work)
        last_work = NULL;

    uacpi_work_handler handler = w->handler;
    uacpi_handle context = w->context;

    working = true;
    handler(context);
    working = false;
    return true;
}

static void worker(void *unused) {
    (void)unused;
    for (;;) {
        while (run_next_work())
            ;
        process_wakeup(&working);
        process_sleep_uninterruptible(&first_work, 0);
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    if (!have_worker || !process_can_sleep()) {
        while (run_next_work())
            ;
        return UACPI_STATUS_OK;
    }
    while (first_work || working)
        process_sleep_uninterruptible(&working, 0);
    return UACPI_STATUS_OK;
}

void acpi_start_worker(void) {
    have_worker = process_spawn_kernel_thread(worker, NULL, "acpi") == 0;
}
