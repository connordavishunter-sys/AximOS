#include <acpi.h>
#include <ahci.h>
#include <ata.h>
#include <cpu.h>
#include <e1000.h>
#include <audio.h>
#include <io.h>
#include <irq.h>
#include <keyboard.h>
#include <mm.h>
#include <net.h>
#include <process.h>
#include <random.h>
#include <serial.h>
#include <usb.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIC_INIT 0x11
#define PIC_8086_MODE 0x01
#define PIC_CASCADE_IRQ 2
#define PIC1_VECTORS 0x20
#define PIC2_VECTORS 0x28

#define PIT_COMMAND 0x43
#define PIT_CHANNEL2 0x42
#define PIT_GATE_PORT 0x61
#define PIT_GATE 0x01
#define PIT_SPEAKER 0x02
#define PIT_OUTPUT 0x20
#define PIT_CHANNEL2_ONE_SHOT 0xB0
#define PIT_FREQUENCY 1193182
#define TSC_MEASURE_HZ 20 // the longest count the PIT can do is about 55 ms
#define WAIT_LIMIT 5000000 // reads of a timer, seconds longer than a wait takes
#define MIN_TSC_HZ 200000000ULL // slower than any x86-64 CPU, so a timer that says so isn't counting

#define HPET_CAPABILITIES 0x000
#define HPET_CONFIG 0x010
#define HPET_COUNTER 0x0F0
#define HPET_ENABLE 0x1
#define FS_PER_SECOND 1000000000000000ULL

#define LAPIC_ID 0x20
#define LAPIC_EOI 0xB0
#define LAPIC_SPURIOUS 0xF0
#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310
#define LAPIC_TIMER 0x320
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE 0x3E0

#define LAPIC_ENABLE 0x100
#define LAPIC_TIMER_PERIODIC 0x20000
#define LAPIC_TIMER_MASKED 0x10000
#define LAPIC_DIVIDE_BY_16 0x3
#define ICR_DELIVERY_PENDING 0x1000
#define ICR_DELIVERY_NMI 0x400

#define IOAPIC_SELECT 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_REDIRECTION_BASE 0x10
#define IOAPIC_ACTIVE_LOW 0x2000
#define IOAPIC_LEVEL 0x8000

#define MSI_ADDRESS_BASE 0xFEE00000
#define MSI_DESTINATION_SHIFT 12

static volatile uint8_t *lapic;
static volatile uint8_t *ioapic;
static uint32_t timer_count;
static uint32_t boot_apic_id;
static volatile uint64_t ticks;
static uint64_t tsc_hz;
static bool pit_missing;
static volatile uint8_t *hpet;

static uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic + reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(lapic + reg) = value;
}

static void ioapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(ioapic + IOAPIC_SELECT) = reg;
    *(volatile uint32_t *)(ioapic + IOAPIC_WINDOW) = value;
}

// Moves the old PIC's vectors away from the CPU exceptions and masks everything.
// Its spurious interrupts could otherwise still look like exceptions.
static void pic_disable(void) {
    outb(PIC1_COMMAND, PIC_INIT);
    outb(PIC2_COMMAND, PIC_INIT);
    outb(PIC1_DATA, PIC1_VECTORS);
    outb(PIC2_DATA, PIC2_VECTORS);
    outb(PIC1_DATA, 1 << PIC_CASCADE_IRQ);
    outb(PIC2_DATA, PIC_CASCADE_IRQ);
    outb(PIC1_DATA, PIC_8086_MODE);
    outb(PIC2_DATA, PIC_8086_MODE);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// Starts the PIT counting down. Its output rises when it reaches zero.
static void pit_start(uint16_t pit_count) {
    uint8_t gate = inb(PIT_GATE_PORT);

    outb(PIT_GATE_PORT, (gate & ~PIT_SPEAKER) | PIT_GATE);
    outb(PIT_COMMAND, PIT_CHANNEL2_ONE_SHOT);
    outb(PIT_CHANNEL2, pit_count & 0xFF);
    outb(PIT_CHANNEL2, pit_count >> 8);

    // Restarting the gate is what starts the count.
    gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, gate & ~PIT_GATE);
    outb(PIT_GATE_PORT, gate | PIT_GATE);
}

// Times a count of the PIT in timestamp counts. Newer chipsets can leave the PIT
// switched off, so that its output never rises, or rises at once.
static bool pit_wait(uint64_t hz, uint64_t *tsc_counts) {
    if (pit_missing)
        return false;
    pit_start(PIT_FREQUENCY / hz);

    uint64_t start = __builtin_ia32_rdtsc();

    for (int i = 0; i < WAIT_LIMIT; i++) {
        if (inb(PIT_GATE_PORT) & PIT_OUTPUT) {
            *tsc_counts = __builtin_ia32_rdtsc() - start;
            pit_missing = *tsc_counts * hz < MIN_TSC_HZ;
            return !pit_missing;
        }
    }
    pit_missing = true;
    return false;
}

// The same by the HPET's counter, which is left running.
static bool hpet_wait(uint64_t hz, uint64_t *tsc_counts) {
    if (!hpet) {
        uint64_t phys = acpi_hpet_address();

        if (!phys || !(hpet = mm_map_device(phys, PAGE_SIZE)))
            return false;
        kprintf("irq: the PIT doesn't count, so timing goes by the HPET\n");
        *(volatile uint64_t *)(hpet + HPET_CONFIG) |= HPET_ENABLE;
    }

    uint64_t period_fs = *(volatile uint64_t *)(hpet + HPET_CAPABILITIES) >> 32;

    if (!period_fs)
        return false;

    volatile uint32_t *counter = (volatile uint32_t *)(hpet + HPET_COUNTER);
    uint32_t counts = FS_PER_SECOND / period_fs / hz, begin = *counter;
    uint64_t start = __builtin_ia32_rdtsc();

    for (int i = 0; i < WAIT_LIMIT; i++) {
        if ((uint32_t)(*counter - begin) >= counts) {
            *tsc_counts = __builtin_ia32_rdtsc() - start;
            return *tsc_counts * hz >= MIN_TSC_HZ;
        }
    }
    return false;
}

static bool reference_wait(uint64_t hz, uint64_t *tsc_counts) {
    return pit_wait(hz, tsc_counts) || hpet_wait(hz, tsc_counts);
}

// What Intel CPUs since 2017 say about their clocks: the crystal that drives the
// APIC timer, and how many timestamp counts there are to each of its cycles.
// Virtual machines' timers run at their own speed, so this is only for when
// there's nothing to measure them by.
static bool cpuid_clocks(uint64_t *crystal_hz, uint64_t *tsc_hz) {
    uint32_t top, denominator, numerator, crystal, unused;

    __asm__ volatile("cpuid" : "=a"(top), "=b"(unused), "=c"(unused), "=d"(unused) : "a"(0));
    if (top < 0x15)
        return false;
    __asm__ volatile("cpuid" : "=a"(denominator), "=b"(numerator), "=c"(crystal), "=d"(unused) : "a"(0x15), "c"(0));
    if (!denominator || !numerator)
        return false;

    uint64_t hz = crystal;

    // Some leave the crystal out, but say what the counter runs at in MHz.
    if (!hz && top >= 0x16) {
        uint32_t base_mhz;

        __asm__ volatile("cpuid" : "=a"(base_mhz), "=b"(unused), "=c"(unused), "=d"(unused) : "a"(0x16), "c"(0));
        hz = (uint64_t)base_mhz * 1000000 * denominator / numerator;
    }
    if (!hz)
        return false;
    *crystal_hz = hz;
    *tsc_hz = hz * numerator / denominator;
    return true;
}

uint64_t irq_tsc_hz(void) {
    return tsc_hz;
}

// Counts how fast the local APIC timer runs, by the timestamp counter.
static uint32_t measure_timer(void) {
    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);

    uint64_t start = __builtin_ia32_rdtsc();

    while (__builtin_ia32_rdtsc() - start < tsc_hz / TIMER_HZ)
        ;

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);

    lapic_write(LAPIC_TIMER_INITIAL, 0);
    return elapsed;
}

// The timestamp counter is measured by another timer, then the APIC timer by it.
// Failing that, Intel CPUs say how fast both go.
static bool measure_clocks(void) {
    uint64_t counts, crystal;

    if (reference_wait(TSC_MEASURE_HZ, &counts)) {
        tsc_hz = counts * TSC_MEASURE_HZ;
        timer_count = measure_timer();
    } else if (cpuid_clocks(&crystal, &tsc_hz)) {
        kprintf("irq: there's no timer to measure the others by, so going by the CPU's %lu Hz crystal\n", crystal);
        timer_count = crystal / 16 / TIMER_HZ;
    } else {
        kprintf("irq: there's no timer to measure the others by\n");
    }
    return timer_count != 0;
}

void irq_init_this_cpu(void) {
    lapic_write(LAPIC_SPURIOUS, LAPIC_ENABLE | VECTOR_SPURIOUS);
    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_PERIODIC | VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INITIAL, timer_count);
}

bool irq_init(void) {
    lapic = mm_map_device(acpi_local_apic_address(), PAGE_SIZE);
    ioapic = mm_map_device(acpi_io_apic_address(), PAGE_SIZE);
    if (!lapic || !ioapic)
        return false;

    pic_disable();
    lapic_write(LAPIC_SPURIOUS, LAPIC_ENABLE | VECTOR_SPURIOUS);

    boot_apic_id = lapic_read(LAPIC_ID) >> 24;
    this_cpu()->apic_id = boot_apic_id;
    if (!measure_clocks())
        return false;

    kprintf("irq: timer counts %u per tick\n", timer_count);
    irq_init_this_cpu();
    return true;
}

void irq_enable_isa(int irq) {
    struct acpi_isa_route route = acpi_isa_route(irq);
    uint32_t low = (IRQ_ISA_BASE + irq) | (route.active_low ? IOAPIC_ACTIVE_LOW : 0)
        | (route.level_triggered ? IOAPIC_LEVEL : 0);
    uint32_t reg = IOAPIC_REDIRECTION_BASE + route.gsi * 2;

    ioapic_write(reg + 1, boot_apic_id << 24);
    ioapic_write(reg, low);
}

void irq_enable_line(int line, int vector) {
    if (line == 0 || line >= 16)
        return;

    struct acpi_isa_route route = acpi_isa_route(line);
    uint32_t reg = IOAPIC_REDIRECTION_BASE + route.gsi * 2;

    ioapic_write(reg + 1, boot_apic_id << 24);
    ioapic_write(reg, vector | (route.active_low ? IOAPIC_ACTIVE_LOW : 0) | IOAPIC_LEVEL);
}

void irq_handle(const struct interrupt_frame *frame) {
    uint64_t vector = frame->vector;

    if (vector == VECTOR_SPURIOUS)
        return;

    random_add_interrupt(vector);

    if (vector == IRQ_ISA_BASE + IRQ_KEYBOARD || vector == IRQ_ISA_BASE + IRQ_MOUSE) {
        keyboard_handle_irq();
    } else if (vector == IRQ_ISA_BASE + IRQ_ATA_PRIMARY) {
        ata_handle_irq(0);
    } else if (vector == IRQ_ISA_BASE + IRQ_ATA_SECONDARY) {
        ata_handle_irq(1);
    } else if (vector == VECTOR_AHCI) {
        ahci_handle_irq();
    } else if (vector == VECTOR_USB) {
        xhci_handle_irq();
    } else if (vector == VECTOR_NET) {
        e1000_poll();
    } else if (vector == VECTOR_TIMER && this_cpu()->index == 0) {
        ticks++;
        process_wake_expired();
        net_tick();
        audio_tick();
        keyboard_tick();
        xhci_tick();
    } else if (vector >= IRQ_ISA_BASE && vector < IRQ_ISA_BASE + 16) {
        acpi_handle_irq(vector - IRQ_ISA_BASE);
    }

    if (vector == VECTOR_TIMER && !this_cpu()->running)
        this_cpu()->idle_ticks++;

    // Acknowledge first, as this may not return until the process runs again.
    lapic_write(LAPIC_EOI, 0);

    // The kernel only takes interrupts while a CPU is idle, and the idle loop is
    // already looking for something to run.
    if (vector == VECTOR_TIMER && (frame->cs & 3) == 3) {
        process_charge_tick();
        schedule();
    }
}

uint64_t irq_ticks(void) {
    return ticks;
}

static void send_ipi(uint32_t destination, uint32_t command) {
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING)
        __asm__ volatile("pause");

    lapic_write(LAPIC_ICR_HIGH, destination << 24);
    lapic_write(LAPIC_ICR_LOW, command);
}

void irq_send_wake(uint32_t apic_id) {
    send_ipi(apic_id, VECTOR_WAKE);
}

void irq_send_nmi(uint32_t apic_id) {
    if (lapic)
        send_ipi(apic_id, ICR_DELIVERY_NMI);
}

void irq_send_flush(uint32_t apic_id) {
    send_ipi(apic_id, VECTOR_FLUSH);
}

void irq_acknowledge(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t irq_msi_address(void) {
    return MSI_ADDRESS_BASE | boot_apic_id << MSI_DESTINATION_SHIFT;
}
