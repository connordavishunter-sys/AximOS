#pragma once

// Finds AHCI controllers and registers their drives as sda and up, or as sr0 and
// up for CD drives.
void ahci_init(void);

void ahci_handle_irq(void);
