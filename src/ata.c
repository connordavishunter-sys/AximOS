#include <ata.h>
#include <block.h>
#include <io.h>
#include <irq.h>
#include <process.h>
#include <serial.h>
#include <string.h>

// PIO with 28-bit LBA, so only the first 128 GiB of a disk is reachable. Once
// processes are running a command sleeps until the drive's interrupt arrives,
// though the data itself is still moved by the CPU.

#define REG_FEATURES 1
#define REG_COUNT 2
#define REG_LBA_LOW 3
#define REG_LBA_MID 4
#define REG_LBA_HIGH 5
#define REG_DRIVE 6
#define REG_STATUS 7
#define REG_COMMAND 7

#define STATUS_ERR 0x01
#define STATUS_DRQ 0x08
#define STATUS_DF 0x20
#define STATUS_BSY 0x80

#define CMD_READ 0x20
#define CMD_WRITE 0x30
#define CMD_FLUSH 0xE7
#define CMD_PACKET 0xA0
#define CMD_IDENTIFY_PACKET 0xA1
#define CMD_IDENTIFY 0xEC

#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ 0x28

#define ATAPI_SIGNATURE_MID 0x14
#define ATAPI_SIGNATURE_HIGH 0xEB
#define ATAPI_RETRIES 3

#define CTRL_NO_IRQ 0x02
#define TIMEOUT 1000000
#define MAX_SECTORS 128
#define IRQ_TIMEOUT_TICKS TIMER_HZ
#define CHANNEL_COUNT 2
#define POSITIONS 4
#define SECTOR_SIZE 512
#define ATAPI_SECTOR_SIZE 2048

struct channel {
    uint16_t io;
    uint16_t ctrl;
    volatile bool irq_fired;
    bool irq_enabled;
};

struct ata_drive {
    struct block_device block;
    struct channel *channel;
    uint8_t slave;
    bool atapi;
};

static struct channel channels[CHANNEL_COUNT] = {{.io = 0x1F0, .ctrl = 0x3F6}, {.io = 0x170, .ctrl = 0x376}};
static struct ata_drive drives[POSITIONS];

// Reading the control port four times gives the drive its required 400ns.
static void settle(const struct channel *c) {
    for (int i = 0; i < 4; i++)
        inb(c->ctrl);
}

static bool wait_not_busy(const struct channel *c) {
    for (int i = 0; i < TIMEOUT; i++) {
        if (!(inb(c->io + REG_STATUS) & STATUS_BSY))
            return true;
    }
    return false;
}

static bool wait_data(const struct channel *c) {
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t status = inb(c->io + REG_STATUS);

        if (status & STATUS_BSY)
            continue;
        if (status & (STATUS_ERR | STATUS_DF))
            return false;
        if (status & STATUS_DRQ)
            return true;
    }
    return false;
}

static bool can_sleep(const struct channel *c) {
    return c->irq_enabled && process_can_sleep();
}

// Interrupts must be on or off before a command starts, and need a sleeping
// process to wake up. During boot it's just polling.
static void prepare_command(struct channel *c) {
    c->irq_fired = false;
    outb(c->ctrl, can_sleep(c) ? 0 : CTRL_NO_IRQ);
}

// Sleeps until the drive has finished its current step. If the interrupt never
// comes, the polling that follows is still enough to carry on.
static void wait_for_irq(struct channel *c) {
    if (!can_sleep(c))
        return;

    uint64_t deadline = irq_ticks() + IRQ_TIMEOUT_TICKS;

    while (!c->irq_fired && irq_ticks() < deadline)
        process_sleep_uninterruptible(c, IRQ_TIMEOUT_TICKS);

    if (!c->irq_fired)
        kprintf("ata: interrupt timed out\n");
}

void ata_handle_irq(int index) {
    struct channel *c = &channels[index];

    // Reading the status register is what acknowledges the drive.
    inb(c->io + REG_STATUS);
    c->irq_fired = true;
    process_wakeup(c);
}

static void log_model(const uint16_t *id) {
    kprintf("ata: model \"");
    for (int i = 27; i < 47; i++)
        kprintf("%c%c", id[i] >> 8, id[i] & 0xFF);
    kprintf("\"\n");
}

// Sends a SCSI command and reads its reply, whose size is at most len.
static bool atapi_command(struct ata_drive *d, const uint8_t packet[12], void *buf, uint16_t len) {
    struct channel *c = d->channel;

    if (!wait_not_busy(c))
        return false;

    prepare_command(c);
    outb(c->io + REG_DRIVE, 0xA0 | d->slave << 4);
    settle(c);

    outb(c->io + REG_FEATURES, 0);
    outb(c->io + REG_LBA_MID, len & 0xFF);
    outb(c->io + REG_LBA_HIGH, len >> 8);
    outb(c->io + REG_COMMAND, CMD_PACKET);
    settle(c);

    // The drive asks for the packet without an interrupt, and quickly.
    if (!wait_data(c))
        return false;

    c->irq_fired = false;
    outsw(c->io, packet, 6);
    settle(c);

    wait_for_irq(c);
    if (!wait_data(c))
        return false;

    uint16_t available = inb(c->io + REG_LBA_MID) | inb(c->io + REG_LBA_HIGH) << 8;

    c->irq_fired = false;
    insw(c->io, buf, (available < len ? available : len) / 2);

    // Another interrupt reports the end of the command.
    wait_for_irq(c);
    return wait_not_busy(c);
}

// Drives report an error for the first command after a disc change, so retry.
static bool atapi_retry(struct ata_drive *d, const uint8_t packet[12], void *buf, uint16_t len) {
    for (int i = 0; i < ATAPI_RETRIES; i++) {
        if (atapi_command(d, packet, buf, len))
            return true;
    }
    return false;
}

static bool start_transfer(struct ata_drive *d, uint32_t lba, uint32_t count, uint8_t command) {
    struct channel *c = d->channel;

    if (!wait_not_busy(c))
        return false;

    prepare_command(c);
    outb(c->io + REG_DRIVE, 0xE0 | d->slave << 4 | lba >> 24);
    settle(c);

    outb(c->io + REG_COUNT, count);
    outb(c->io + REG_LBA_LOW, lba);
    outb(c->io + REG_LBA_MID, lba >> 8);
    outb(c->io + REG_LBA_HIGH, lba >> 16);
    outb(c->io + REG_COMMAND, command);
    settle(c);
    return true;
}

// The drive raises an interrupt for each sector of a command, once it's ready to move.
static bool read_disk(struct block_device *device, uint32_t lba, uint32_t count, void *buf) {
    struct ata_drive *d = device->driver;
    struct channel *c = d->channel;
    uint8_t *out = buf;

    if (!start_transfer(d, lba, count, CMD_READ))
        return false;

    for (uint32_t i = 0; i < count; i++) {
        wait_for_irq(c);
        if (!wait_data(c))
            return false;

        c->irq_fired = false;
        insw(c->io, out + i * SECTOR_SIZE, SECTOR_SIZE / 2);
    }
    return true;
}

static bool write_disk(struct block_device *device, uint32_t lba, uint32_t count, const void *buf) {
    struct ata_drive *d = device->driver;
    struct channel *c = d->channel;
    const uint8_t *in = buf;

    if (!start_transfer(d, lba, count, CMD_WRITE) || !wait_data(c))
        return false;

    for (uint32_t i = 0; i < count; i++) {
        c->irq_fired = false;
        outsw(c->io, in + i * SECTOR_SIZE, SECTOR_SIZE / 2);
        settle(c);

        wait_for_irq(c);
        if (!wait_not_busy(c) || (inb(c->io + REG_STATUS) & (STATUS_ERR | STATUS_DF)))
            return false;
        if (i + 1 < count && !wait_data(c))
            return false;
    }
    return true;
}

static void flush_disk(struct block_device *device) {
    struct ata_drive *d = device->driver;
    struct channel *c = d->channel;

    if (!wait_not_busy(c))
        return;

    prepare_command(c);
    outb(c->io + REG_DRIVE, 0xE0 | d->slave << 4);
    settle(c);
    outb(c->io + REG_COMMAND, CMD_FLUSH);
    settle(c);

    wait_for_irq(c);
    wait_not_busy(c);
}

static bool read_disc(struct block_device *device, uint32_t lba, uint32_t count, void *buf) {
    (void)count; // discs say they take one at a time

    const uint8_t packet[12] = {SCSI_READ, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, 1};

    return atapi_retry(device->driver, packet, buf, ATAPI_SECTOR_SIZE);
}

static void read_disc_capacity(struct ata_drive *d) {
    const uint8_t packet[12] = {SCSI_READ_CAPACITY};
    uint8_t reply[8] __attribute__((aligned(2)));

    if (atapi_retry(d, packet, reply, sizeof(reply)))
        d->block.sector_count = ((uint32_t)reply[0] << 24 | reply[1] << 16 | reply[2] << 8 | reply[3]) + 1;
    else
        kprintf("ata: no disc in drive\n");
}

static bool identify(struct ata_drive *d) {
    struct channel *c = d->channel;
    uint16_t id[256];

    outb(c->ctrl, CTRL_NO_IRQ);
    outb(c->io + REG_DRIVE, 0xA0 | d->slave << 4);
    settle(c);

    outb(c->io + REG_COUNT, 0);
    outb(c->io + REG_LBA_LOW, 0);
    outb(c->io + REG_LBA_MID, 0);
    outb(c->io + REG_LBA_HIGH, 0);
    outb(c->io + REG_COMMAND, CMD_IDENTIFY);
    settle(c);

    uint8_t status = inb(c->io + REG_STATUS);
    if (status == 0 || status == 0xFF || !wait_not_busy(c))
        return false;

    uint8_t mid = inb(c->io + REG_LBA_MID);
    uint8_t high = inb(c->io + REG_LBA_HIGH);

    d->atapi = mid == ATAPI_SIGNATURE_MID && high == ATAPI_SIGNATURE_HIGH;
    if (d->atapi) {
        outb(c->io + REG_COMMAND, CMD_IDENTIFY_PACKET);
        settle(c);
    } else if (mid || high) {
        kprintf("ata: skipping unknown device\n");
        return false;
    }
    if (!wait_data(c))
        return false;

    insw(c->io, id, 256);
    log_model(id);

    if (!d->atapi) {
        d->block.sector_count = id[60] | (uint32_t)id[61] << 16;
        if (d->block.sector_count == 0) {
            kprintf("ata: skipping disk without LBA support\n");
            return false;
        }
    }
    return true;
}

void ata_init(void) {
    for (int position = 0; position < POSITIONS; position++) {
        struct ata_drive *d = &drives[position];
        struct block_device *b = &d->block;

        d->channel = &channels[position / CHANNEL_COUNT];
        d->slave = position % CHANNEL_COUNT;

        kprintf("ata: probing channel %u drive %u\n", position / CHANNEL_COUNT, d->slave);
        if (!identify(d))
            continue;

        b->driver = d;
        b->optical = d->atapi;

        if (d->atapi) {
            b->sector_size = ATAPI_SECTOR_SIZE;
            b->max_sectors = 1;
            b->read = read_disc;
            read_disc_capacity(d);
            memcpy(b->name, "sr0", 4);
            b->name[2] += block_next_optical_index();
        } else {
            b->sector_size = SECTOR_SIZE;
            b->max_sectors = MAX_SECTORS;
            b->read = read_disk;
            b->write = write_disk;
            b->flush = flush_disk;
            memcpy(b->name, "hda", 4);
            b->name[2] += position;
        }

        if (block_register(b) && !d->channel->irq_enabled) {
            d->channel->irq_enabled = true;
            irq_enable_isa(d->channel == &channels[0] ? IRQ_ATA_PRIMARY : IRQ_ATA_SECONDARY);
        }
    }
}
