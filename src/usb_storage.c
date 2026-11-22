#include <block.h>
#include <kmalloc.h>
#include <mm.h>
#include <serial.h>
#include <string.h>
#include <usb.h>

// USB sticks, card readers and drives that take SCSI commands in bulk-only wrappers:
// a command block out, the data, then a status back. Only the first LUN is used.

#define DESCRIPTOR_INTERFACE 4
#define DESCRIPTOR_ENDPOINT 5
#define CLASS_STORAGE 8
#define PROTOCOL_BULK_ONLY 0x50
#define SET_CONFIGURATION 9
#define ENDPOINT_BULK 2

#define CBW_SIGNATURE 0x43425355
#define CSW_SIGNATURE 0x53425355
#define CBW_IN 0x80
#define CSW_OFFSET 64

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ 0x28
#define SCSI_WRITE 0x2A
#define SCSI_SYNCHRONIZE 0x35
#define TYPE_CD 5

#define READY_TRIES 50 // 100 ms apart, for drives still spinning up
#define TRANSFER_MAX (USB_BULK_PAGES * PAGE_SIZE)

struct cbw {
    uint32_t signature, tag, length;
    uint8_t flags, lun, command_length, command[16];
} __attribute__((packed));

struct csw {
    uint32_t signature, tag, residue;
    uint8_t status;
} __attribute__((packed));

struct usb_disk {
    struct block_device block;
    struct usb_device *usb;
    uint8_t *pages[USB_BULK_PAGES]; // the data moves through these
    uint8_t *wrappers;              // and the command and status through this
    uint32_t tag;
};

static bool scsi(struct usb_disk *d, const uint8_t *command, uint8_t length, uint32_t bytes, bool in) {
    struct cbw *cbw = (struct cbw *)d->wrappers;
    struct csw *csw = (struct csw *)(d->wrappers + CSW_OFFSET);
    uint8_t *status = (uint8_t *)csw;

    *cbw = (struct cbw){CBW_SIGNATURE, ++d->tag, bytes, in ? CBW_IN : 0, 0, length, {0}};
    memcpy(cbw->command, command, length);
    memset(csw, 0, sizeof(*csw));
    if (!usb_bulk(d->usb, false, &d->wrappers, sizeof(*cbw)))
        return false;

    // A failed data stage still has a status after it, once the stall is cleared.
    bool moved = !bytes || usb_bulk(d->usb, in, d->pages, bytes);

    if (!usb_bulk(d->usb, true, &status, sizeof(*csw)) && !usb_bulk(d->usb, true, &status, sizeof(*csw)))
        return false;
    return moved && csw->signature == CSW_SIGNATURE && csw->tag == d->tag && csw->status == 0;
}

static void copy_out(const struct usb_disk *d, void *buf, uint32_t bytes) {
    for (uint32_t i = 0; i * PAGE_SIZE < bytes; i++)
        memcpy((uint8_t *)buf + i * PAGE_SIZE, d->pages[i], bytes - i * PAGE_SIZE < PAGE_SIZE ? bytes - i * PAGE_SIZE : PAGE_SIZE);
}

static void copy_in(struct usb_disk *d, const void *buf, uint32_t bytes) {
    for (uint32_t i = 0; i * PAGE_SIZE < bytes; i++)
        memcpy(d->pages[i], (const uint8_t *)buf + i * PAGE_SIZE, bytes - i * PAGE_SIZE < PAGE_SIZE ? bytes - i * PAGE_SIZE : PAGE_SIZE);
}

static bool transfer(struct usb_disk *d, uint8_t op, uint32_t lba, uint32_t count) {
    const uint8_t command[10] = {op, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count};

    return scsi(d, command, sizeof(command), count * d->block.sector_size, op == SCSI_READ);
}

static bool read_sectors(struct block_device *device, uint32_t lba, uint32_t count, void *buf) {
    struct usb_disk *d = device->driver;

    if (!transfer(d, SCSI_READ, lba, count))
        return false;
    copy_out(d, buf, count * device->sector_size);
    return true;
}

static bool write_sectors(struct block_device *device, uint32_t lba, uint32_t count, const void *buf) {
    struct usb_disk *d = device->driver;

    copy_in(d, buf, count * device->sector_size);
    return transfer(d, SCSI_WRITE, lba, count);
}

static void flush(struct block_device *device) {
    const uint8_t command[10] = {SCSI_SYNCHRONIZE};

    scsi(device->driver, command, sizeof(command), 0, false);
}

// Most need asking until they're ready, and to be told what went wrong in between.
static bool wait_ready(struct usb_disk *d) {
    const uint8_t ready[6] = {SCSI_TEST_UNIT_READY}, sense[6] = {SCSI_REQUEST_SENSE, 0, 0, 0, 18};

    for (int i = 0; i < READY_TRIES; i++) {
        if (scsi(d, ready, sizeof(ready), 0, false))
            return true;
        scsi(d, sense, sizeof(sense), 18, true);
        usb_delay_ms(100);
    }
    return false;
}

static void set_capacity(struct usb_disk *d) {
    const uint8_t command[10] = {SCSI_READ_CAPACITY};
    const uint8_t *reply = d->pages[0];

    if (!wait_ready(d) || !scsi(d, command, sizeof(command), 8, true)) {
        kprintf("usb: %s is empty\n", d->block.name);
        return;
    }

    uint32_t last = (uint32_t)reply[0] << 24 | reply[1] << 16 | reply[2] << 8 | reply[3];

    d->block.sector_size = (uint32_t)reply[4] << 24 | reply[5] << 16 | reply[6] << 8 | reply[7];
    d->block.sector_count = last == UINT32_MAX ? last : last + 1; // sector numbers are 32 bits
}

bool usb_storage_probe(struct usb_device *usb, const uint8_t *config, uint16_t length) {
    uint8_t in = 0, out = 0;
    uint16_t in_packet = 0, out_packet = 0;
    bool ours = false;

    for (uint16_t at = 0; at + 7 <= length && config[at] >= 2 && !(in && out); at += config[at]) {
        const uint8_t *desc = &config[at];

        if (desc[1] == DESCRIPTOR_INTERFACE)
            ours = desc[3] == 0 && desc[5] == CLASS_STORAGE && desc[7] == PROTOCOL_BULK_ONLY;
        else if (ours && desc[1] == DESCRIPTOR_ENDPOINT && (desc[3] & 3) == ENDPOINT_BULK) {
            if (desc[2] & 0x80)
                in = desc[2], in_packet = desc[4] | (desc[5] & 0x7) << 8;
            else
                out = desc[2], out_packet = desc[4] | (desc[5] & 0x7) << 8;
        }
    }
    if (!in || !out)
        return false;

    struct usb_disk *d = kmalloc(sizeof(*d));

    for (int i = 0; d && i <= USB_BULK_PAGES; i++) {
        uint64_t page = mm_alloc_kernel_page();

        if (!page) {
            d = NULL;
            break;
        }
        *(i < USB_BULK_PAGES ? &d->pages[i] : &d->wrappers) = mm_phys_to_virt(page);
    }
    if (!d) {
        kprintf("usb: out of memory\n");
        return true;
    }
    d->usb = usb;
    d->tag = 0;
    if (!usb_control(usb, 0, SET_CONFIGURATION, config[5], 0, NULL, 0)
        || !usb_open_bulk(usb, in, in_packet, out, out_packet)) {
        kprintf("usb: storage won't start\n");
        return true;
    }

    const uint8_t inquiry[6] = {SCSI_INQUIRY, 0, 0, 0, 36};
    const char *reply = (const char *)d->pages[0];
    struct block_device *b = &d->block;

    if (!scsi(d, inquiry, sizeof(inquiry), 36, true)) {
        kprintf("usb: storage doesn't say what it is\n");
        return true;
    }
    memset(b, 0, sizeof(*b));
    b->driver = d;
    b->optical = (reply[0] & 0x1F) == TYPE_CD;
    if (b->optical) {
        memcpy(b->name, "sr0", 4);
        b->name[2] += block_next_optical_index();
    } else {
        block_name_disk(b);
        b->write = write_sectors;
        b->flush = flush;
    }
    b->read = read_sectors;
    char product[25] = {0};

    memcpy(product, reply + 8, 24); // the vendor, then the product
    kprintf("usb: %s is %s\n", b->name, product);
    set_capacity(d);
    if (!b->sector_size)
        b->sector_size = b->optical ? 2048 : 512;
    b->max_sectors = TRANSFER_MAX / b->sector_size;
    block_register(b);
    return true;
}
