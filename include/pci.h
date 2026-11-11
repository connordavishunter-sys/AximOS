#pragma once

#include <stdint.h>

#define PCI_ID 0x00
#define PCI_CLASS 0x08
#define PCI_BAR0 0x10
#define PCI_INTERRUPT 0x3C

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

uint32_t pci_read(struct pci_device device, int offset);
void pci_write(struct pci_device device, int offset, uint32_t value);

// Calls found for every function of every device.
void pci_for_each(void (*found)(struct pci_device device));

// Lets a device use its memory registers and reach memory itself.
void pci_enable(struct pci_device device);

// Has the device raise a vector on the boot CPU, returning false if it can't.
bool pci_enable_msi(struct pci_device device, uint8_t vector);
