#include <ahci.h>
#include <block.h>
#include <io.h>
#include <irq.h>
#include <kmalloc.h>
#include <mm.h>
#include <pci.h>
#include <process.h>
#include <serial.h>
#include <string.h>

// One command at a time per port, moving a sector through a buffer the device
// can reach by DMA. The VFS lock already means only one command runs at a time.

#define PCI_BAR5 0x24
#define PCI_CLASS_AHCI 0x010601

#define HBA_SIZE 0x1100
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define GHC_INTERRUPTS 0x2
#define GHC_AHCI 0x80000000
#define MAX_PORTS 32

#define PORT_BASE 0x100
#define PORT_SIZE 0x80
#define PORT_CLB 0x00
#define PORT_CLBU 0x04
#define PORT_FB 0x08
#define PORT_FBU 0x0C
#define PORT_IS 0x10
#define PORT_IE 0x14
#define PORT_CMD 0x18
#define PORT_TFD 0x20
#define PORT_SIG 0x24
#define PORT_SSTS 0x28
#define PORT_SERR 0x30
#define PORT_CI 0x38

#define CMD_START 0x0001
#define CMD_FIS_RECEIVE 0x0010
#define CMD_FIS_RUNNING 0x4000
#define CMD_LIST_RUNNING 0x8000
#define IS_TASK_FILE_ERROR (1u << 30)
#define IS_ENABLED_EVENTS (IS_TASK_FILE_ERROR | 0x3)
#define SSTS_PRESENT 0x3
#define SSTS_ACTIVE 0x1
#define TFD_BUSY_OR_DRQ 0x88
#define SIGNATURE_ATAPI 0xEB140101

#define FIS_TYPE_H2D 0x27
#define FIS_COMMAND 0x80
#define FIS_LBA_MODE 0x40
#define FIS_FEATURE_DMA 0x01

#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT 0xEA
#define ATA_PACKET 0xA0
#define ATA_IDENTIFY_PACKET 0xA1
#define ATA_IDENTIFY 0xEC
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ 0x28

#define HEADER_FIS_DWORDS 5
#define HEADER_ATAPI 0x20
#define HEADER_WRITE 0x40

#define CLB_OFFSET 0
#define FB_OFFSET 1024
#define TABLE_OFFSET 2048
#define ACMD_OFFSET 0x40
#define PRDT_OFFSET 0x80

#define SECTOR_SIZE 512
#define ATAPI_SECTOR_SIZE 2048
#define ATAPI_RETRIES 3
#define START_TIMEOUT 1000000
#define POLL_TIMEOUT 100000000
#define COMMAND_TIMEOUT_TICKS (5 * TIMER_HZ)
#define IDENTIFY_WORDS_LBA48 100
#define BUFFER_PAGES 16
#define MAX_SECTORS (BUFFER_PAGES * PAGE_SIZE / SECTOR_SIZE)

struct command_header {
    uint16_t flags;
    uint16_t table_entries;
    uint32_t transferred;
    uint32_t table_low;
    uint32_t table_high;
    uint32_t reserved[4];
};

struct prdt_entry {
    uint32_t address_low;
    uint32_t address_high;
    uint32_t reserved;
    uint32_t byte_count; // minus one
};

struct ahci_port {
    struct block_device block;
    volatile uint8_t *registers;
    uint8_t *memory;       // the command list, received FISes and command table
    uint64_t memory_phys;
    uint8_t *buffer[BUFFER_PAGES]; // the pages data moves through
    bool atapi;
    volatile bool done;
    volatile bool failed; // the command that ran last, as the interrupt saw
};

static struct ahci_port *ports[MAX_PORTS];
static int port_count;
static volatile uint8_t *hba;
static bool have_interrupts;
static int disk_count;

static uint32_t reg_read(const struct ahci_port *p, uint32_t reg) {
    return *(volatile uint32_t *)(p->registers + reg);
}

static void reg_write(const struct ahci_port *p, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(p->registers + reg) = value;
}

// Runs one command in slot 0, sleeping until the port's interrupt if it can.
static bool run_command(struct ahci_port *p, uint8_t command, uint64_t lba, const uint8_t *packet,
                        uint32_t bytes, bool write) {
    struct command_header *header = (struct command_header *)(p->memory + CLB_OFFSET);
    uint8_t *table = p->memory + TABLE_OFFSET;
    struct prdt_entry *prdt = (struct prdt_entry *)(table + PRDT_OFFSET);

    for (int i = 0; i < POLL_TIMEOUT && (reg_read(p, PORT_TFD) & TFD_BUSY_OR_DRQ); i++)
        ;

    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    memset(table, 0, PRDT_OFFSET + BUFFER_PAGES * sizeof(*prdt));
    table[0] = FIS_TYPE_H2D;
    table[1] = FIS_COMMAND;
    table[2] = command;
    table[4] = lba;
    table[5] = lba >> 8;
    table[6] = lba >> 16;
    table[7] = FIS_LBA_MODE;
    table[8] = lba >> 24;
    table[9] = lba >> 32;
    table[10] = lba >> 40;
    table[12] = !packet && bytes >= SECTOR_SIZE ? bytes / SECTOR_SIZE : 1;
    table[13] = !packet && bytes >= SECTOR_SIZE ? bytes / SECTOR_SIZE >> 8 : 0;

    if (packet) {
        table[3] = FIS_FEATURE_DMA;
        memcpy(table + ACMD_OFFSET, packet, 12);
    }

    header->flags = HEADER_FIS_DWORDS | (packet ? HEADER_ATAPI : 0) | (write ? HEADER_WRITE : 0);
    header->table_entries = pages;
    header->transferred = 0;
    header->table_low = p->memory_phys + TABLE_OFFSET;
    header->table_high = (p->memory_phys + TABLE_OFFSET) >> 32;

    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = mm_virt_to_phys(p->buffer[i]);
        uint32_t part = i + 1 < pages ? PAGE_SIZE : bytes - i * PAGE_SIZE;

        prdt[i].address_low = phys;
        prdt[i].address_high = phys >> 32;
        prdt[i].byte_count = part - 1;
    }

    p->done = p->failed = false;
    reg_write(p, PORT_IS, 0xFFFFFFFF);
    reg_write(p, PORT_CI, 1);

    bool sleeping = have_interrupts && process_can_sleep();
    uint64_t deadline = irq_ticks() + COMMAND_TIMEOUT_TICKS;

    for (int i = 0; i < POLL_TIMEOUT; i++) {
        if (p->failed || reg_read(p, PORT_IS) & IS_TASK_FILE_ERROR)
            return false;
        if (!(reg_read(p, PORT_CI) & 1))
            return true;

        if (sleeping) {
            if (irq_ticks() >= deadline)
                break;
            if (!p->done)
                process_sleep_uninterruptible(p, COMMAND_TIMEOUT_TICKS);
            p->done = false;
        }
    }
    kprintf("ahci: %s command %x timed out\n", p->block.name, command);
    return false;
}

void ahci_handle_irq(void) {
    for (int i = 0; i < port_count; i++) {
        struct ahci_port *p = ports[i];
        uint32_t events = reg_read(p, PORT_IS);

        // Every bit is cleared, as one left set would raise the interrupt again
        // and again, and an error is remembered for the command waiting.
        if (events) {
            reg_write(p, PORT_IS, events);
            p->failed |= events & IS_TASK_FILE_ERROR;
            p->done = true;
            process_wakeup(p);
        }
    }
    *(volatile uint32_t *)(hba + HBA_IS) = *(volatile uint32_t *)(hba + HBA_IS);
}

static bool read_disk(struct block_device *device, uint32_t lba, uint32_t count, void *buf) {
    struct ahci_port *p = device->driver;
    uint32_t bytes = count * SECTOR_SIZE;
    uint8_t *out = buf;

    if (!run_command(p, ATA_READ_DMA_EXT, lba, NULL, bytes, false))
        return false;

    for (uint32_t i = 0; i * PAGE_SIZE < bytes; i++) {
        uint32_t left = bytes - i * PAGE_SIZE;

        memcpy(out + i * PAGE_SIZE, p->buffer[i], left < PAGE_SIZE ? left : PAGE_SIZE);
    }
    return true;
}

static bool write_disk(struct block_device *device, uint32_t lba, uint32_t count, const void *buf) {
    struct ahci_port *p = device->driver;
    uint32_t bytes = count * SECTOR_SIZE;
    const uint8_t *in = buf;

    for (uint32_t i = 0; i * PAGE_SIZE < bytes; i++) {
        uint32_t left = bytes - i * PAGE_SIZE;

        memcpy(p->buffer[i], in + i * PAGE_SIZE, left < PAGE_SIZE ? left : PAGE_SIZE);
    }
    return run_command(p, ATA_WRITE_DMA_EXT, lba, NULL, bytes, true);
}

static void flush_disk(struct block_device *device) {
    run_command(device->driver, ATA_FLUSH_EXT, 0, NULL, 0, false);
}

// Drives report an error for the first command after a disc change, so retry.
static bool packet_command(struct ahci_port *p, const uint8_t packet[12], uint32_t bytes) {
    for (int i = 0; i < ATAPI_RETRIES; i++) {
        if (run_command(p, ATA_PACKET, 0, packet, bytes, false))
            return true;
    }
    return false;
}

static bool read_disc(struct block_device *device, uint32_t lba, uint32_t count, void *buf) {
    struct ahci_port *p = device->driver;

    (void)count; // discs say they take one at a time
    const uint8_t packet[12] = {SCSI_READ, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, 1};

    if (!packet_command(p, packet, ATAPI_SECTOR_SIZE))
        return false;

    memcpy(buf, p->buffer[0], ATAPI_SECTOR_SIZE);
    return true;
}

static bool wait_cmd_clear(const struct ahci_port *p, uint32_t bits) {
    for (int i = 0; i < START_TIMEOUT; i++) {
        if (!(reg_read(p, PORT_CMD) & bits))
            return true;
    }
    return false;
}

static void init_port(volatile uint8_t *registers) {
    if (port_count == MAX_PORTS)
        return;

    struct ahci_port *p = kmalloc(sizeof(*p));
    uint64_t memory = mm_alloc_kernel_page();

    if (!p || !memory) {
        kprintf("ahci: out of memory\n");
        return;
    }
    for (int i = 0; i < BUFFER_PAGES; i++) {
        uint64_t page = mm_alloc_kernel_page();

        if (!page) {
            kprintf("ahci: out of memory\n");
            return;
        }
        p->buffer[i] = mm_phys_to_virt(page);
    }

    p->registers = registers;
    p->memory_phys = memory;
    p->memory = mm_phys_to_virt(memory);
    p->atapi = reg_read(p, PORT_SIG) == SIGNATURE_ATAPI;

    reg_write(p, PORT_CMD, reg_read(p, PORT_CMD) & ~(uint32_t)(CMD_START | CMD_FIS_RECEIVE));
    if (!wait_cmd_clear(p, CMD_LIST_RUNNING | CMD_FIS_RUNNING)) {
        kprintf("ahci: port won't stop\n");
        return;
    }

    reg_write(p, PORT_CLB, memory + CLB_OFFSET);
    reg_write(p, PORT_CLBU, (memory + CLB_OFFSET) >> 32);
    reg_write(p, PORT_FB, memory + FB_OFFSET);
    reg_write(p, PORT_FBU, (memory + FB_OFFSET) >> 32);
    reg_write(p, PORT_SERR, 0xFFFFFFFF);
    reg_write(p, PORT_IS, 0xFFFFFFFF);
    reg_write(p, PORT_CMD, reg_read(p, PORT_CMD) | CMD_FIS_RECEIVE | CMD_START);

    if (!run_command(p, p->atapi ? ATA_IDENTIFY_PACKET : ATA_IDENTIFY, 0, NULL, SECTOR_SIZE, false)) {
        kprintf("ahci: identify failed\n");
        return;
    }

    struct block_device *b = &p->block;
    const uint16_t *id = (const uint16_t *)p->buffer[0];

    b->driver = p;
    b->optical = p->atapi;

    if (p->atapi) {
        const uint8_t packet[12] = {SCSI_READ_CAPACITY};

        b->sector_size = ATAPI_SECTOR_SIZE;
        b->max_sectors = 1;
        b->read = read_disc;
        memcpy(b->name, "sr0", 4);
        b->name[2] += block_next_optical_index();

        if (packet_command(p, packet, 8)) {
            const uint8_t *reply = p->buffer[0];
            b->sector_count = ((uint32_t)reply[0] << 24 | reply[1] << 16 | reply[2] << 8 | reply[3]) + 1;
        } else {
            kprintf("ahci: no disc in drive\n");
        }
    } else {
        uint64_t sectors = id[IDENTIFY_WORDS_LBA48] | (uint64_t)id[IDENTIFY_WORDS_LBA48 + 1] << 16
            | (uint64_t)id[IDENTIFY_WORDS_LBA48 + 2] << 32;

        if (sectors == 0)
            sectors = id[60] | (uint32_t)id[61] << 16;

        // Sector numbers are 32 bits in the rest of the kernel.
        b->sector_count = sectors > 0xFFFFFFFF ? 0xFFFFFFFF : sectors;
        b->sector_size = SECTOR_SIZE;
        b->max_sectors = MAX_SECTORS;
        b->read = read_disk;
        b->write = write_disk;
        b->flush = flush_disk;
        memcpy(b->name, "sda", 4);
        b->name[2] += disk_count++;
    }

    ports[port_count++] = p;
    block_register(b);
    reg_write(p, PORT_IE, IS_ENABLED_EVENTS);
}

static void init_controller(struct pci_device device) {
    pci_enable(device);

    uint64_t base = pci_read(device, PCI_BAR5) & ~0xFULL;

    hba = mm_map_device(base, HBA_SIZE);
    if (!hba) {
        kprintf("ahci: can't map controller\n");
        return;
    }

    volatile uint32_t *ghc = (volatile uint32_t *)(hba + HBA_GHC);

    *ghc |= GHC_AHCI;

    uint32_t implemented = *(volatile uint32_t *)(hba + HBA_PI);

    kprintf("ahci: controller at %lx, ports %x\n", base, implemented);

    for (int i = 0; i < MAX_PORTS; i++) {
        volatile uint8_t *registers = hba + PORT_BASE + i * PORT_SIZE;

        if (!(implemented & 1u << i))
            continue;

        uint32_t status = *(volatile uint32_t *)(registers + PORT_SSTS);

        if ((status & 0xF) == SSTS_PRESENT && (status >> 8 & 0xF) == SSTS_ACTIVE)
            init_port(registers);
    }

    // PCI interrupt lines need ACPI's routing tables to use, but MSI just needs an
    // address and vector. Without it, commands fall back to polling.
    have_interrupts = pci_enable_msi(device, VECTOR_AHCI);
    *ghc |= GHC_INTERRUPTS;
}

static void found_device(struct pci_device device) {
    // Only the first controller's interrupts are handled for now.
    if (pci_read(device, PCI_CLASS) >> 8 == PCI_CLASS_AHCI && !hba)
        init_controller(device);
}

void ahci_init(void) {
    pci_for_each(found_device);
}
