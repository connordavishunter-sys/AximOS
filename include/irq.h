#pragma once

#include <interrupt.h>

// ISA interrupts arrive at IRQ_ISA_BASE plus their number.
#define IRQ_ISA_BASE 32
#define IRQ_KEYBOARD 1
#define IRQ_MOUSE 12
#define IRQ_ATA_PRIMARY 14
#define IRQ_ATA_SECONDARY 15

#define VECTOR_TIMER 48
#define VECTOR_WAKE 49
#define VECTOR_AHCI 50
#define VECTOR_FLUSH 51
#define VECTOR_NET 52
#define VECTOR_USB 53
#define VECTOR_SPURIOUS 0xFF

// How many timer ticks there are in a second.
#define TIMER_HZ 100

// Moves interrupts from the old PIC to the APICs and starts this CPU's timer.
// Interrupts stay disabled until the first return to ring 3 or idle loop.
bool irq_init(void);

// The rest of what irq_init does, for the other CPUs.
void irq_init_this_cpu(void);

// Routes an ISA interrupt, which starts out masked, to the boot CPU.
void irq_enable_isa(int irq);

// Routes a PCI device's interrupt line, level triggered, to the boot CPU.
void irq_enable_line(int line, int vector);

void irq_handle(const struct interrupt_frame *frame);

// How fast the CPU's timestamp counter runs, as irq_init measured it.
uint64_t irq_tsc_hz(void);

// Ticks since boot, for timeouts.
uint64_t irq_ticks(void);

// Interrupts an idle CPU so it looks for something to run.
void irq_send_wake(uint32_t apic_id);

void irq_send_nmi(uint32_t apic_id);

// Asks a CPU to run cpu_handle_flush.
void irq_send_flush(uint32_t apic_id);

// Tells the local APIC an interrupt has been dealt with.
void irq_acknowledge(void);

// The address and data a PCI device's MSI uses to raise a vector on the boot CPU.
uint32_t irq_msi_address(void);
