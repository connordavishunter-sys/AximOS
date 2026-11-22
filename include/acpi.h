#pragma once

#include <stdint.h>

// How an ISA interrupt reaches the I/O APIC, which isn't always straight across.
struct acpi_isa_route {
    uint32_t gsi;
    bool active_low;
    bool level_triggered;
};

// Finds the tables the kernel needs. Returns false if they can't be found.
bool acpi_init(void);

uint64_t acpi_local_apic_address(void);
uint64_t acpi_io_apic_address(void);
uint64_t acpi_hpet_address(void); // or 0 if there's no HPET
struct acpi_isa_route acpi_isa_route(int irq);

// Turns the machine off or restarts it using what the firmware described.
// These only return if they fail.
void acpi_power_off(void);

// Starts uACPI, which runs the firmware's own code: switching the machine into
// ACPI mode, setting up its devices, and from then on the power button, the lid
// and powering off. Needs interrupts routed and the clock going.
void acpi_start(void);

// Starts the thread that the firmware's code runs on when something happens.
void acpi_start_worker(void);

// For an ISA interrupt nothing else took, which may be the SCI. Returns whether it was.
bool acpi_handle_irq(int irq);

// Whether the lid is open, or -1 without one.
int acpi_lid_open(void);

// For uACPI's kernel functions.
uint64_t acpi_rsdp_address(void);
void acpi_route_sci(int irq);
void acpi_reset(void);
