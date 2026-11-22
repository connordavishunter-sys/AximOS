#pragma once

#include <stdint.h>

#define USB_BULK_PAGES 16 // the most one bulk transfer moves, in pages

// A device on a root port of an xHCI controller. Hubs aren't supported.
struct usb_device;

// Finds xHCI controllers, sets up what's plugged in at boot, and hands mass storage
// devices to usb_storage_probe.
void xhci_init(void);

void xhci_handle_irq(void);

// Waits without sleeping, for setting up at boot.
void usb_delay_ms(uint64_t ms);

// A request on endpoint 0. Data is copied through a page the controller can reach,
// so length can be up to 2048.
bool usb_control(struct usb_device *device, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                 void *data, uint16_t length);

// Opens a pair of bulk endpoints, by address and largest packet.
bool usb_open_bulk(struct usb_device *device, uint8_t in, uint16_t in_packet, uint8_t out, uint16_t out_packet);

// Moves up to USB_BULK_PAGES pages of data through pages from mm_alloc_kernel_page.
// A stalled endpoint is cleared again, but the transfer still fails.
bool usb_bulk(struct usb_device *device, bool in, uint8_t *const *pages, uint32_t bytes);

// Looks at a device's configuration descriptor and, if it's storage it can drive,
// takes it on. Returns false to leave the device alone.
bool usb_storage_probe(struct usb_device *device, const uint8_t *config, uint16_t length);
