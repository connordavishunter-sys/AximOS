#include <io.h>
#include <irq.h>
#include <pci.h>

#define PCI_ADDRESS 0xCF8
#define PCI_DATA 0xCFC
#define PCI_ENABLE 0x80000000
#define PCI_COMMAND 0x04
#define PCI_HEADER 0x0C
#define PCI_CAPABILITIES 0x34
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_BUS_MASTER 0x4
#define PCI_STATUS_HAS_CAPABILITIES (1 << 20)
#define PCI_HEADER_MULTIFUNCTION 0x800000
#define PCI_CAP_MSI 0x05
#define MSI_ENABLE (1 << 16)
#define MSI_64_BIT (1 << 23)

static void select_register(struct pci_device d, int offset) {
    outl(PCI_ADDRESS, PCI_ENABLE | d.bus << 16 | d.device << 11 | d.function << 8 | offset);
}

uint32_t pci_read(struct pci_device d, int offset) {
    select_register(d, offset);
    return inl(PCI_DATA);
}

void pci_write(struct pci_device d, int offset, uint32_t value) {
    select_register(d, offset);
    outl(PCI_DATA, value);
}

void pci_for_each(void (*found)(struct pci_device device)) {
    for (int bus = 0; bus < 256; bus++) {
        for (int device = 0; device < 32; device++) {
            struct pci_device d = {bus, device, 0};

            if ((pci_read(d, PCI_ID) & 0xFFFF) == 0xFFFF)
                continue;

            int functions = pci_read(d, PCI_HEADER) & PCI_HEADER_MULTIFUNCTION ? 8 : 1;

            for (d.function = 0; d.function < functions; d.function++) {
                if ((pci_read(d, PCI_ID) & 0xFFFF) != 0xFFFF)
                    found(d);
            }
        }
    }
}

void pci_enable(struct pci_device d) {
    pci_write(d, PCI_COMMAND, (pci_read(d, PCI_COMMAND) & 0xFFFF) | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);
}

bool pci_enable_msi(struct pci_device d, uint8_t vector) {
    if (!(pci_read(d, PCI_COMMAND) & PCI_STATUS_HAS_CAPABILITIES))
        return false;

    int offset = pci_read(d, PCI_CAPABILITIES) & 0xFC;

    while (offset) {
        uint32_t cap = pci_read(d, offset);

        if ((cap & 0xFF) == PCI_CAP_MSI) {
            pci_write(d, offset + 4, irq_msi_address());
            if (cap & MSI_64_BIT) {
                pci_write(d, offset + 8, 0);
                pci_write(d, offset + 12, vector);
            } else {
                pci_write(d, offset + 8, vector);
            }
            pci_write(d, offset, cap | MSI_ENABLE);
            return true;
        }
        offset = cap >> 8 & 0xFC;
    }
    return false;
}
