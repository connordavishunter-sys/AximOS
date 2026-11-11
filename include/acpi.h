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
struct acpi_isa_route acpi_isa_route(int irq);

// Turns the machine off or restarts it using what the firmware described.
// These only return if they fail.
void acpi_power_off(void);
void acpi_reset(void);
