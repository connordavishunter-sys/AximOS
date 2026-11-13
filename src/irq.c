#include <acpi.h>
#include <ahci.h>
#include <ata.h>
#include <cpu.h>
#include <e1000.h>
#include <hda.h>
#include <io.h>
#include <irq.h>
#include <keyboard.h>
#include <mm.h>
#include <net.h>
#include <process.h>
#include <random.h>
#include <serial.h>

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

static void pit_wait(void) {
    while (!(inb(PIT_GATE_PORT) & PIT_OUTPUT))
        ;
}

uint64_t irq_measure_tsc_hz(void) {
    pit_start(PIT_FREQUENCY / TSC_MEASURE_HZ);

    uint64_t start = __builtin_ia32_rdtsc();

    pit_wait();
    return (__builtin_ia32_rdtsc() - start) * TSC_MEASURE_HZ;
}

// Counts how fast the local APIC timer runs against the PIT's known frequency.
static uint32_t measure_timer(void) {
    pit_start(PIT_FREQUENCY / TIMER_HZ);

    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_DIVIDE_BY_16);
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);

    pit_wait();

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);

    lapic_write(LAPIC_TIMER_INITIAL, 0);
    return elapsed;
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
    timer_count = measure_timer();
    if (timer_count == 0)
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
    } else if (vector == VECTOR_NET) {
        e1000_poll();
    } else if (vector == VECTOR_TIMER && this_cpu()->index == 0) {
        ticks++;
        process_wake_expired();
        net_tick();
        hda_tick();
        keyboard_tick();
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
