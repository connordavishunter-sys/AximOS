#include <clock.h>
#include <irq.h>
#include <kmalloc.h>
#include <mm.h>
#include <pci.h>
#include <process.h>
#include <serial.h>
#include <string.h>
#include <usb.h>

// USB 3 controllers, set up once at boot for the devices on their root ports. Each
// ring is one page, and one transfer or command runs at a time, which the VFS lock
// already makes so, waiting for the event that says it finished. Interrupt endpoints,
// such as a keyboard's, are the exception: each always has a transfer waiting, and
// its events are handed straight to its driver.

#define PCI_CLASS_XHCI 0x0C0330
#define BAR_64_BIT 0x4
#define MMIO_SIZE 0x10000 // more than its registers need, and the rest is never touched
#define MAX_CONTROLLERS 4
#define MAX_SLOTS 32
#define MAX_PIPES 16 // interrupt endpoints on each controller
#define SAVED_EVENTS 8 // what the interrupt handler finds for whoever's waiting
#define TIMEOUT_NS (5 * NS_PER_SECOND)

#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18
#define HCC_PORT_POWER 0x8
#define HCC_CONTEXT_64 0x4

#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(i) (0x400 + (i) * 0x10)
#define USBCMD_RUN 0x1
#define USBCMD_RESET 0x2
#define USBCMD_INTERRUPTS 0x4
#define USBSTS_HALTED 0x1
#define USBSTS_EVENT 0x8
#define USBSTS_NOT_READY 0x800

#define PORT_CONNECTED 0x1
#define PORT_ENABLED 0x2
#define PORT_RESET 0x10
#define PORT_POWER 0x200
#define PORT_SPEED(sc) ((sc) >> 10 & 0xF)
#define PORT_CHANGES 0xFE0000
#define PORT_RESET_CHANGE 0x200000
#define SPEED_FULL 1
#define SPEED_LOW 2
#define SPEED_HIGH 3
#define SPEED_SUPER 4

#define INTERRUPTER 0x20 // the first, in the runtime registers
#define IMAN 0x00
#define IMOD 0x04
#define ERSTSZ 0x08
#define ERSTBA 0x10
#define ERDP 0x18
#define IMAN_PENDING 0x1
#define IMAN_ENABLE 0x2
#define ERDP_BUSY 0x8

#define LEGACY_SUPPORT 1
#define LEGACY_BIOS_OWNED 0x10000
#define LEGACY_OS_OWNED 0x1000000
#define LEGACY_SMI_ENABLES 0xE1FE0
#define LEGACY_SMI_EVENTS 0xE0000000

#define TRB_CYCLE 0x1
#define TRB_TOGGLE 0x2
#define TRB_SHORT_OK 0x4 // report a short packet, which ends the transfer
#define TRB_CHAIN 0x10
#define TRB_IOC 0x20
#define TRB_IMMEDIATE 0x40
#define TRB_IN 0x10000
#define TRB_TYPE(t) ((t) << 10)
#define TRB_SLOT(s) ((uint32_t)(s) << 24)
#define TRB_ENDPOINT(e) ((uint32_t)(e) << 16)
#define RING_TRBS (PAGE_SIZE / sizeof(struct trb))

enum {
    NORMAL = 1,
    SETUP = 2,
    DATA = 3,
    STATUS = 4,
    LINK = 6,
    ENABLE_SLOT = 9,
    ADDRESS_DEVICE = 11,
    CONFIGURE_ENDPOINT = 12,
    EVALUATE_CONTEXT = 13,
    RESET_ENDPOINT = 14,
    SET_DEQUEUE = 16,
    TRANSFER_EVENT = 32,
};

#define CODE_SUCCESS 1
#define CODE_STALL 6
#define CODE_SHORT 13

#define EP_BULK_OUT 2
#define EP_CONTROL 4
#define EP_BULK_IN 6
#define EP_INTERRUPT_IN 7

#define GET_DESCRIPTOR 6
#define CLEAR_FEATURE 1
#define DEVICE_DESCRIPTOR 0x100
#define CONFIG_DESCRIPTOR 0x200
#define CONTROL_MAX 2048

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    volatile struct trb *trbs;
    uint64_t phys;
    uint32_t index;
    uint32_t cycle;
};

// An interrupt endpoint, whose reports go to handler as they come.
struct pipe {
    struct usb_device *device;
    uint8_t id;
    struct ring ring;
    uint8_t *buffer;
    uint16_t packet;
    usb_report_handler handler;
    void *context;
};

struct xhci {
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *doorbells;
    int ports;
    size_t context_size;
    uint64_t *dcbaa;
    struct ring commands;
    volatile struct trb *events;
    uint64_t events_phys;
    uint32_t event_index, event_cycle;
    bool have_interrupts;
    volatile bool interrupted;
    struct pipe pipes[MAX_PIPES];
    int pipe_count;
    struct trb saved[SAVED_EVENTS];
    int saved_count;
};

struct usb_device {
    struct xhci *hc;
    uint8_t slot, port, speed;
    uint8_t *input; // the input context, kept for configuring endpoints later
    uint64_t input_phys;
    uint8_t *buffer; // where control data goes through
    struct ring control, in, out;
    uint8_t in_id, out_id, in_address, out_address;
    uint8_t last_id; // the highest endpoint the slot has
};

static struct xhci *controllers[MAX_CONTROLLERS];
static int controller_count;

static uint32_t r32(volatile uint8_t *base, uint32_t reg) {
    return *(volatile uint32_t *)(base + reg);
}

static void w32(volatile uint8_t *base, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(base + reg) = value;
}

static void w64(volatile uint8_t *base, uint32_t reg, uint64_t value) {
    w32(base, reg, value);
    w32(base, reg + 4, value >> 32);
}

void usb_delay_ms(uint64_t ms) {
    for (uint64_t until = clock_monotonic_ns() + ms * 1000000; clock_monotonic_ns() < until;)
        ;
}

static bool wait_until(volatile uint8_t *base, uint32_t reg, uint32_t mask, uint32_t value, uint64_t ms) {
    for (uint64_t until = clock_monotonic_ns() + ms * 1000000; clock_monotonic_ns() < until;) {
        if ((r32(base, reg) & mask) == value)
            return true;
    }
    return false;
}

static void *alloc_page(uint64_t *phys) {
    uint64_t page = mm_alloc_kernel_page();

    if (phys)
        *phys = page;
    return page ? mm_phys_to_virt(page) : NULL;
}

// The last TRB links back to the first, flipping the cycle bit that says which
// TRBs are new.
static bool ring_init(struct ring *r) {
    if (!(r->trbs = alloc_page(&r->phys)))
        return false;
    r->index = 0;
    r->cycle = 1;
    r->trbs[RING_TRBS - 1].param = r->phys;
    return true;
}

static uint64_t ring_position(const struct ring *r) {
    return r->phys + r->index * sizeof(struct trb);
}

// Adds a TRB, returning its address, which the event about it will name.
static uint64_t push(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    volatile struct trb *t = &r->trbs[r->index];
    uint64_t at = ring_position(r);

    t->param = param;
    t->status = status;
    t->control = control | r->cycle; // last, as the controller may start on it at once
    if (++r->index == RING_TRBS - 1) {
        r->trbs[r->index].control = TRB_TYPE(LINK) | TRB_TOGGLE | (control & TRB_CHAIN) | r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
    return at;
}

static uint8_t code_of(const struct trb *event) {
    return event->status >> 24;
}

static uint8_t type_of(const struct trb *event) {
    return event->control >> 10 & 0x3F;
}

static bool next_event(struct xhci *hc, struct trb *event) {
    volatile struct trb *e = &hc->events[hc->event_index];

    if ((e->control & TRB_CYCLE) != hc->event_cycle)
        return false;
    *event = (struct trb){e->param, e->status, e->control};
    if (++hc->event_index == RING_TRBS) {
        hc->event_index = 0;
        hc->event_cycle ^= 1;
    }
    w64(hc->rt, INTERRUPTER + ERDP, (hc->events_phys + hc->event_index * sizeof(struct trb)) | ERDP_BUSY);
    return true;
}

static void queue_report(struct xhci *hc, struct pipe *p) {
    push(&p->ring, mm_virt_to_phys(p->buffer), p->packet, TRB_TYPE(NORMAL) | TRB_SHORT_OK | TRB_IOC);
    hc->doorbells[p->device->slot] = p->id;
}

// Hands a report to its driver and waits for the next, returning false if the event
// isn't about an interrupt endpoint. One that fails is left stopped.
static bool deliver_report(struct xhci *hc, const struct trb *event) {
    if (type_of(event) != TRANSFER_EVENT)
        return false;
    for (struct pipe *p = hc->pipes; p < hc->pipes + hc->pipe_count; p++) {
        if (p->device->slot != event->control >> 24 || p->id != (event->control >> 16 & 0x1F))
            continue;
        if (code_of(event) == CODE_SUCCESS || code_of(event) == CODE_SHORT) {
            p->handler(p->context, p->buffer, p->packet - (event->status & 0xFFFFFF));
            queue_report(hc, p);
        } else {
            kprintf("xhci: port %u stopped reporting, code %u\n", p->device->port, code_of(event));
        }
        return true;
    }
    return false;
}

// Waits for the event about the TRB at trb, or a transfer that went wrong, which can
// only be the one running. Events about anything else, such as ports, are dropped.
static bool wait_for(struct xhci *hc, uint64_t trb, struct trb *event) {
    bool sleeping = hc->have_interrupts && process_can_sleep();

    for (uint64_t until = clock_monotonic_ns() + TIMEOUT_NS; clock_monotonic_ns() < until;) {
        if (hc->saved_count) {
            *event = hc->saved[0];
            memmove(hc->saved, hc->saved + 1, --hc->saved_count * sizeof(*event));
        } else if (!next_event(hc, event)) {
            if (sleeping && !hc->interrupted)
                process_sleep_uninterruptible(hc, TIMER_HZ);
            hc->interrupted = false;
            continue;
        } else if (deliver_report(hc, event)) {
            continue;
        }
        if (event->param == trb || (type_of(event) == TRANSFER_EVENT && code_of(event) != CODE_SUCCESS))
            return true;
    }
    kprintf("xhci: timed out\n");
    return false;
}

// Reports go to their drivers, and anything else is kept for wait_for.
static void drain(struct xhci *hc) {
    struct trb event;

    while (next_event(hc, &event)) {
        if (deliver_report(hc, &event))
            continue;
        if (hc->saved_count == SAVED_EVENTS)
            memmove(hc->saved, hc->saved + 1, --hc->saved_count * sizeof(event));
        hc->saved[hc->saved_count++] = event;
    }
    hc->interrupted = true;
    process_wakeup(hc);
}

void xhci_handle_irq(void) {
    for (int i = 0; i < controller_count; i++) {
        struct xhci *hc = controllers[i];

        if (r32(hc->op, OP_USBSTS) & USBSTS_EVENT) {
            w32(hc->op, OP_USBSTS, USBSTS_EVENT);
            w32(hc->rt, INTERRUPTER + IMAN, IMAN_PENDING | IMAN_ENABLE);
            drain(hc);
        }
    }
}

void xhci_tick(void) {
    for (int i = 0; i < controller_count; i++) {
        if (!controllers[i]->have_interrupts && controllers[i]->pipe_count)
            drain(controllers[i]);
    }
}

static bool command(struct xhci *hc, uint64_t param, uint32_t control, uint8_t *slot) {
    uint64_t at = push(&hc->commands, param, 0, control);
    struct trb event;

    hc->doorbells[0] = 0;
    if (!wait_for(hc, at, &event) || code_of(&event) != CODE_SUCCESS)
        return false;
    if (slot)
        *slot = event.control >> 24;
    return true;
}

// The input context's parts: what to change, then the slot, then each endpoint by id.
static uint32_t *context(const struct usb_device *d, int part) {
    return (uint32_t *)(d->input + part * d->hc->context_size);
}

static void set_endpoint(struct usb_device *d, uint8_t id, uint32_t type, uint16_t packet, const struct ring *r) {
    uint32_t *ep = context(d, 1 + id);

    memset(ep, 0, d->hc->context_size);
    ep[1] = 3 << 1 | type << 3 | (uint32_t)packet << 16; // retrying errors 3 times
    ep[2] = r->phys | r->cycle;
    ep[3] = r->phys >> 32;
    ep[4] = type == EP_CONTROL ? 8 : PAGE_SIZE; // the average transfer, as a hint
}

// After a stall, the endpoint starts again after what's on its ring.
static void restart_endpoint(struct usb_device *d, uint8_t id, const struct ring *r) {
    uint32_t control = TRB_SLOT(d->slot) | TRB_ENDPOINT(id);

    command(d->hc, 0, TRB_TYPE(RESET_ENDPOINT) | control, NULL);
    command(d->hc, ring_position(r) | r->cycle, TRB_TYPE(SET_DEQUEUE) | control, NULL);
}

bool usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                 void *data, uint16_t length) {
    bool in = type & 0x80;
    uint64_t setup = type | request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 | (uint64_t)length << 48;
    struct trb event;

    if (length > CONTROL_MAX)
        return false;
    if (!in && length)
        memcpy(d->buffer, data, length);
    push(&d->control, setup, 8, TRB_TYPE(SETUP) | TRB_IMMEDIATE | (length ? (in ? 3 : 2) << 16 : 0));
    if (length)
        push(&d->control, mm_virt_to_phys(d->buffer), length, TRB_TYPE(DATA) | (in ? TRB_IN : 0));

    uint64_t at = push(&d->control, 0, 0, TRB_TYPE(STATUS) | TRB_IOC | (length && in ? 0 : TRB_IN));

    d->hc->doorbells[d->slot] = 1;
    if (!wait_for(d->hc, at, &event))
        return false;
    if (code_of(&event) != CODE_SUCCESS) {
        if (code_of(&event) == CODE_STALL)
            restart_endpoint(d, 1, &d->control);
        return false;
    }
    if (in && length)
        memcpy(data, d->buffer, length);
    return true;
}

bool usb_open_bulk(struct usb_device *d, uint8_t in, uint16_t in_packet, uint8_t out, uint16_t out_packet) {
    uint32_t *slot = context(d, 1);

    d->in_address = in;
    d->out_address = out;
    d->in_id = (in & 0xF) * 2 + 1;
    d->out_id = (out & 0xF) * 2;
    if (!ring_init(&d->in) || !ring_init(&d->out))
        return false;
    context(d, 0)[0] = 0;
    context(d, 0)[1] = 1 | 1u << d->in_id | 1u << d->out_id;
    d->last_id = d->in_id > d->out_id ? d->in_id : d->out_id;
    slot[0] = (slot[0] & 0x07FFFFFF) | (uint32_t)d->last_id << 27;
    set_endpoint(d, d->in_id, EP_BULK_IN, in_packet, &d->in);
    set_endpoint(d, d->out_id, EP_BULK_OUT, out_packet, &d->out);
    return command(d->hc, d->input_phys, TRB_TYPE(CONFIGURE_ENDPOINT) | TRB_SLOT(d->slot), NULL);
}

// How often to ask, as the exponent of 125 µs: bInterval is in frames at full and
// low speeds, and is already an exponent, plus one, faster than that.
static uint8_t interval_exponent(const struct usb_device *d, uint8_t interval) {
    uint8_t e = 3;

    if (d->speed != SPEED_FULL && d->speed != SPEED_LOW)
        return interval ? interval - 1 : 0;
    while (e < 10 && (2u << e) <= interval * 8u)
        e++;
    return e;
}

bool usb_open_interrupt(struct usb_device *d, uint8_t address, uint16_t packet, uint8_t interval,
                        usb_report_handler handler, void *handler_context) {
    struct xhci *hc = d->hc;
    struct pipe *p = &hc->pipes[hc->pipe_count];
    uint8_t id = (address & 0xF) * 2 + 1;

    if (hc->pipe_count == MAX_PIPES || !ring_init(&p->ring) || !(p->buffer = alloc_page(NULL)))
        return false;
    p->device = d;
    p->id = id;
    p->packet = packet < PAGE_SIZE ? packet : PAGE_SIZE;
    p->handler = handler;
    p->context = handler_context;
    if (id > d->last_id)
        d->last_id = id;
    context(d, 0)[0] = 0;
    context(d, 0)[1] = 1 | 1u << id;
    context(d, 1)[0] = (context(d, 1)[0] & 0x07FFFFFF) | (uint32_t)d->last_id << 27;
    set_endpoint(d, id, EP_INTERRUPT_IN, packet, &p->ring);
    context(d, 1 + id)[0] = (uint32_t)interval_exponent(d, interval) << 16;
    context(d, 1 + id)[4] = 8 | (uint32_t)packet << 16; // a small average, and the most each interval
    if (!command(hc, d->input_phys, TRB_TYPE(CONFIGURE_ENDPOINT) | TRB_SLOT(d->slot), NULL))
        return false;
    hc->pipe_count++;
    queue_report(hc, p);
    return true;
}

bool usb_bulk(struct usb_device *d, bool in, uint8_t *const *pages, uint32_t bytes) {
    struct ring *r = in ? &d->in : &d->out;
    uint8_t id = in ? d->in_id : d->out_id;
    uint64_t at = 0;
    struct trb event;

    for (uint32_t done = 0, i = 0; done < bytes && i < USB_BULK_PAGES; i++) {
        uint32_t part = bytes - done < PAGE_SIZE ? bytes - done : PAGE_SIZE;

        done += part;
        at = push(r, mm_virt_to_phys(pages[i]), part,
                  TRB_TYPE(NORMAL) | (in ? TRB_SHORT_OK : 0) | (done < bytes ? TRB_CHAIN : TRB_IOC));
    }
    d->hc->doorbells[d->slot] = id;
    if (!wait_for(d->hc, at, &event))
        return false;
    if (code_of(&event) == CODE_STALL) {
        restart_endpoint(d, id, r);
        usb_control(d, 0x02, CLEAR_FEATURE, 0, in ? d->in_address : d->out_address, NULL, 0); // the halt
    }
    return code_of(&event) == CODE_SUCCESS || code_of(&event) == CODE_SHORT;
}

// Gives the device an address, then reads what it is.
static void probe_port(struct xhci *hc, int port) {
    uint32_t sc = r32(hc->op, OP_PORTSC(port));

    if (!(sc & PORT_CONNECTED))
        return;
    // USB 3 ports enable themselves, but USB 2 ones need a reset first.
    if (!(sc & PORT_ENABLED)) {
        w32(hc->op, OP_PORTSC(port), (sc & ~(PORT_ENABLED | PORT_CHANGES)) | PORT_RESET);
        wait_until(hc->op, OP_PORTSC(port), PORT_RESET_CHANGE, PORT_RESET_CHANGE, 500);
        usb_delay_ms(10);
        sc = r32(hc->op, OP_PORTSC(port));
        if (!(sc & PORT_ENABLED)) {
            kprintf("xhci: port %u won't enable\n", port + 1);
            return;
        }
    }
    w32(hc->op, OP_PORTSC(port), sc & ~PORT_ENABLED); // clearing what changed

    // What's allocated stays, if this fails, as it's only once at boot.
    struct usb_device *d = kmalloc(sizeof(*d));
    uint64_t output_phys;
    uint8_t descriptor[18], header[9];

    if (!d || !(d->input = alloc_page(&d->input_phys)) || !(d->buffer = alloc_page(NULL))
        || !ring_init(&d->control) || !alloc_page(&output_phys)) {
        kprintf("xhci: out of memory\n");
        return;
    }
    d->hc = hc;
    d->port = port + 1;
    d->last_id = 1;
    d->speed = PORT_SPEED(sc);
    if (!command(hc, 0, TRB_TYPE(ENABLE_SLOT), &d->slot) || d->slot > MAX_SLOTS) {
        kprintf("xhci: no slot for port %u\n", d->port);
        return;
    }
    hc->dcbaa[d->slot] = output_phys;

    // Endpoint 0's packets are a guess until the device says, except at USB 3 speeds.
    uint16_t packet = d->speed >= SPEED_SUPER ? 512 : d->speed == SPEED_HIGH ? 64 : 8;

    context(d, 0)[1] = 0x3;
    context(d, 1)[0] = (uint32_t)d->speed << 20 | 1u << 27;
    context(d, 1)[1] = (uint32_t)d->port << 16;
    set_endpoint(d, 1, EP_CONTROL, packet, &d->control);
    if (!command(hc, d->input_phys, TRB_TYPE(ADDRESS_DEVICE) | TRB_SLOT(d->slot), NULL)
        || !usb_control(d, 0x80, GET_DESCRIPTOR, DEVICE_DESCRIPTOR, 0, descriptor, 8)) {
        kprintf("xhci: device on port %u doesn't answer\n", d->port);
        return;
    }
    if (d->speed < SPEED_SUPER && descriptor[7] != packet) {
        context(d, 0)[1] = 0x2;
        context(d, 2)[1] = (context(d, 2)[1] & 0xFFFF) | (uint32_t)descriptor[7] << 16;
        command(hc, d->input_phys, TRB_TYPE(EVALUATE_CONTEXT) | TRB_SLOT(d->slot), NULL);
    }

    uint16_t length;

    if (!usb_control(d, 0x80, GET_DESCRIPTOR, DEVICE_DESCRIPTOR, 0, descriptor, sizeof(descriptor))
        || !usb_control(d, 0x80, GET_DESCRIPTOR, CONFIG_DESCRIPTOR, 0, header, sizeof(header))
        || (length = header[2] | header[3] << 8) < sizeof(header)) {
        kprintf("xhci: device on port %u has no configuration\n", d->port);
        return;
    }
    if (length > CONTROL_MAX)
        length = CONTROL_MAX;

    uint8_t *config = kmalloc(length);

    kprintf("xhci: port %u has device %x:%x\n", d->port, descriptor[8] | descriptor[9] << 8,
            descriptor[10] | descriptor[11] << 8);
    if (config && usb_control(d, 0x80, GET_DESCRIPTOR, CONFIG_DESCRIPTOR, 0, config, length)
        && !usb_storage_probe(d, config, length) && !usb_hid_probe(d, config, length))
        kprintf("xhci: nothing here drives the device on port %u, so it's left alone\n", d->port);
    kfree(config);
}

// The firmware may be using the controller for a USB keyboard, until it's asked.
static void take_from_firmware(struct xhci *hc, uint32_t offset) {
    for (volatile uint32_t *cap = (volatile uint32_t *)hc->cap + offset; offset; cap += offset) {
        if ((*cap & 0xFF) == LEGACY_SUPPORT) {
            *cap |= LEGACY_OS_OWNED;
            wait_until((volatile uint8_t *)cap, 0, LEGACY_BIOS_OWNED, 0, 1000);
            cap[1] = (cap[1] & ~LEGACY_SMI_ENABLES) | LEGACY_SMI_EVENTS;
            return;
        }
        offset = *cap >> 8 & 0xFF;
    }
}

static void init_controller(struct pci_device pci) {
    if (controller_count == MAX_CONTROLLERS)
        return;
    pci_enable(pci);

    uint32_t bar = pci_read(pci, PCI_BAR0);
    uint64_t base = bar & ~0xFULL;
    struct xhci *hc = kmalloc(sizeof(*hc));

    if (bar & BAR_64_BIT)
        base |= (uint64_t)pci_read(pci, PCI_BAR0 + 4) << 32;
    if (hc)
        memset(hc, 0, sizeof(*hc));
    if (!hc || !(hc->cap = mm_map_device(base, MMIO_SIZE))) {
        kprintf("xhci: can't map controller\n");
        return;
    }
    hc->op = hc->cap + hc->cap[0];
    hc->rt = hc->cap + (r32(hc->cap, CAP_RTSOFF) & ~0x1Fu);
    hc->doorbells = (volatile uint32_t *)(hc->cap + (r32(hc->cap, CAP_DBOFF) & ~0x3u));

    uint32_t params1 = r32(hc->cap, CAP_HCSPARAMS1), params2 = r32(hc->cap, CAP_HCSPARAMS2);
    uint32_t features = r32(hc->cap, CAP_HCCPARAMS1);
    int slots = (params1 & 0xFF) < MAX_SLOTS ? params1 & 0xFF : MAX_SLOTS;
    int scratchpads = (params2 >> 21 & 0x1F) << 5 | params2 >> 27;

    hc->ports = params1 >> 24;
    hc->context_size = features & HCC_CONTEXT_64 ? 64 : 32;
    take_from_firmware(hc, features >> 16);

    w32(hc->op, OP_USBCMD, r32(hc->op, OP_USBCMD) & ~USBCMD_RUN);
    wait_until(hc->op, OP_USBSTS, USBSTS_HALTED, USBSTS_HALTED, 100);
    w32(hc->op, OP_USBCMD, USBCMD_RESET);
    if (!wait_until(hc->op, OP_USBCMD, USBCMD_RESET, 0, 1000)
        || !wait_until(hc->op, OP_USBSTS, USBSTS_NOT_READY, 0, 1000)) {
        kprintf("xhci: controller won't reset\n");
        return;
    }

    uint64_t dcbaa_phys, scratch_phys, erst_phys;
    uint64_t *erst = alloc_page(&erst_phys);

    if (!(hc->dcbaa = alloc_page(&dcbaa_phys)) || !erst || !ring_init(&hc->commands)
        || !(hc->events = alloc_page(&hc->events_phys))) {
        kprintf("xhci: out of memory\n");
        return;
    }
    // Pages the controller keeps its own state in.
    if (scratchpads) {
        uint64_t *scratch = alloc_page(&scratch_phys);

        for (int i = 0; scratch && i < scratchpads; i++)
            scratch[i] = mm_alloc_kernel_page();
        hc->dcbaa[0] = scratch_phys;
    }
    w32(hc->op, OP_CONFIG, slots);
    w64(hc->op, OP_DCBAAP, dcbaa_phys);
    w64(hc->op, OP_CRCR, hc->commands.phys | 1);

    erst[0] = hc->events_phys;
    erst[1] = RING_TRBS;
    hc->event_cycle = 1;
    w32(hc->rt, INTERRUPTER + ERSTSZ, 1);
    w64(hc->rt, INTERRUPTER + ERDP, hc->events_phys);
    w64(hc->rt, INTERRUPTER + ERSTBA, erst_phys);
    w32(hc->rt, INTERRUPTER + IMOD, 0);
    w32(hc->rt, INTERRUPTER + IMAN, IMAN_ENABLE);
    controllers[controller_count++] = hc;
    hc->have_interrupts = pci_enable_msi(pci, VECTOR_USB);
    w32(hc->op, OP_USBCMD, USBCMD_RUN | USBCMD_INTERRUPTS);

    for (int i = 0; i < hc->ports && features & HCC_PORT_POWER; i++)
        w32(hc->op, OP_PORTSC(i), (r32(hc->op, OP_PORTSC(i)) & ~(PORT_ENABLED | PORT_CHANGES)) | PORT_POWER);
    kprintf("xhci: controller at %lx, %u ports\n", base, hc->ports);

    usb_delay_ms(100); // for devices to come up after the reset
    for (int i = 0; i < hc->ports; i++)
        probe_port(hc, i);
}

static void found_device(struct pci_device device) {
    if (pci_read(device, PCI_CLASS) >> 8 == PCI_CLASS_XHCI)
        init_controller(device);
}

void xhci_init(void) {
    pci_for_each(found_device);
}
