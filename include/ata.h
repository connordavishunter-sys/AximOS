#pragma once

// Registers every drive on the legacy IDE channels as hda to hdd, or as sr0 and
// up for CD drives.
void ata_init(void);

// Handles the interrupt from a channel, where 0 is primary and 1 is secondary.
void ata_handle_irq(int channel);
