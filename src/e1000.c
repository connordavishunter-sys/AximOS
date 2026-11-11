#include <e1000.h>
#include <irq.h>
#include <mm.h>
#include <net.h>
#include <pci.h>
#include <serial.h>
#include <string.h>

#define REG_CTRL 0x0000
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_IMS 0x00D0
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL 0x5400
#define REG_RAH 0x5404

#define CTRL_SET_LINK_UP (1 << 6)
#define CTRL_RESET (1 << 26)
#define RCTL_ENABLE (1 << 1)
#define RCTL_BROADCAST (1 << 15)
#define RCTL_STRIP_CRC (1 << 26) // and a 2048 byte buffer, which is the zero setting
#define TCTL_ENABLE (1 << 1)
#define TCTL_PAD_SHORT (1 << 3)
#define TCTL_COLLISION_THRESHOLD (0x10 << 4)
#define TCTL_COLLISION_DISTANCE (0x40 << 12)
#define TIPG_DEFAULT 0x0060200A
#define INT_LINK_CHANGE (1 << 2)
#define INT_RX_LOW (1 << 4)
#define INT_RX_OVERRUN (1 << 6)
#define INT_RX_TIMER (1 << 7)
#define EERD_START 1
#define EERD_DONE (1 << 4)
#define EERD_ADDRESS_SHIFT 8
#define EERD_DATA_SHIFT 16

#define DESC_DONE 1
#define DESC_END_OF_PACKET 1
#define RX_END_OF_PACKET (1 << 1)
#define TX_INSERT_CRC (1 << 1)
#define TX_REPORT_STATUS (1 << 3)

#define BUFFER_SIZE 2048
#define BUFFERS_PER_PAGE (PAGE_SIZE / BUFFER_SIZE)
#define RX_COUNT 128
#define TX_COUNT 64
#define REGISTERS_SIZE 0x20000
#define BAR_TYPE_MASK 0x6
#define BAR_TYPE_64_BIT 0x4
#define MAX_RESET_POLLS 1000000

struct rx_desc {
    uint64_t buffer;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t buffer;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} __attribute__((packed));

_Static_assert(RX_COUNT * sizeof(struct rx_desc) <= PAGE_SIZE && TX_COUNT * sizeof(struct tx_desc) <= PAGE_SIZE,
               "each ring has to fit in a page");

static volatile uint8_t *registers;
static struct pci_device card;
static bool found;
static uint8_t mac[6];
static volatile struct rx_desc *rx_ring;
static volatile struct tx_desc *tx_ring;
static uint8_t *rx_buffers[RX_COUNT];
static uint8_t *tx_buffers[TX_COUNT];
static uint32_t rx_next;
static uint32_t tx_next;

static uint32_t reg_read(uint32_t reg) {
    return *(volatile uint32_t *)(registers + reg);
}

static void reg_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(registers + reg) = value;
}

static void found_device(struct pci_device device) {
    static const uint16_t models[] = {0x100E, 0x100F, 0x1015, 0x1019, 0x101E};
    uint32_t id = pci_read(device, PCI_ID);

    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]) && !found; i++) {
        if (id == ((uint32_t)models[i] << 16 | 0x8086)) {
            card = device;
            found = true;
        }
    }
}

static void *new_page(uint64_t *phys) {
    *phys = mm_alloc_kernel_page();
    return *phys ? mm_phys_to_virt(*phys) : NULL;
}

// Gives each descriptor half a page, so a ring's buffers come from count / 2 pages.
static bool alloc_buffers(uint8_t **buffers, uint64_t *addresses, int count) {
    for (int i = 0; i < count; i += BUFFERS_PER_PAGE) {
        uint64_t phys;
        uint8_t *page = new_page(&phys);

        if (!page)
            return false;
        for (int part = 0; part < BUFFERS_PER_PAGE && i + part < count; part++) {
            buffers[i + part] = page + part * BUFFER_SIZE;
            addresses[i + part] = phys + part * BUFFER_SIZE;
        }
    }
    return true;
}

static void read_mac(void) {
    uint32_t low = reg_read(REG_RAL);
    uint32_t high = reg_read(REG_RAH);

    if (low != 0) {
        memcpy(mac, &low, 4);
        memcpy(mac + 4, &high, 2);
        return;
    }

    for (int word = 0; word < 3; word++) {
        reg_write(REG_EERD, word << EERD_ADDRESS_SHIFT | EERD_START);

        uint32_t value;

        for (int poll = 0; poll < MAX_RESET_POLLS && !((value = reg_read(REG_EERD)) & EERD_DONE); poll++)
            ;
        mac[word * 2] = value >> EERD_DATA_SHIFT;
        mac[word * 2 + 1] = value >> (EERD_DATA_SHIFT + 8);
    }
}

bool e1000_init(void) {
    pci_for_each(found_device);
    if (!found)
        return false;

    pci_enable(card);

    uint32_t bar = pci_read(card, PCI_BAR0);
    uint64_t base = bar & ~0xFULL;

    if ((bar & BAR_TYPE_MASK) == BAR_TYPE_64_BIT)
        base |= (uint64_t)pci_read(card, PCI_BAR0 + 4) << 32;
    registers = mm_map_device(base, REGISTERS_SIZE);
    if (!registers)
        return false;

    reg_write(REG_IMC, 0xFFFFFFFF);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RESET);
    for (int poll = 0; poll < MAX_RESET_POLLS && (reg_read(REG_CTRL) & CTRL_RESET); poll++)
        __asm__ volatile("pause");
    reg_write(REG_IMC, 0xFFFFFFFF);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SET_LINK_UP);

    read_mac();
    for (int i = 0; i < 128; i++)
        reg_write(REG_MTA + i * 4, 0);

    uint64_t rx_phys, tx_phys;
    uint64_t addresses[RX_COUNT > TX_COUNT ? RX_COUNT : TX_COUNT];

    rx_ring = new_page(&rx_phys);
    tx_ring = new_page(&tx_phys);
    if (!rx_ring || !tx_ring || !alloc_buffers(rx_buffers, addresses, RX_COUNT))
        return false;
    for (int i = 0; i < RX_COUNT; i++)
        rx_ring[i].buffer = addresses[i];

    if (!alloc_buffers(tx_buffers, addresses, TX_COUNT))
        return false;
    for (int i = 0; i < TX_COUNT; i++) {
        tx_ring[i].buffer = addresses[i];
        tx_ring[i].status = DESC_DONE;
    }

    reg_write(REG_RDBAL, rx_phys);
    reg_write(REG_RDBAH, rx_phys >> 32);
    reg_write(REG_RDLEN, RX_COUNT * sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_COUNT - 1);
    reg_write(REG_RCTL, RCTL_ENABLE | RCTL_BROADCAST | RCTL_STRIP_CRC);

    reg_write(REG_TDBAL, tx_phys);
    reg_write(REG_TDBAH, tx_phys >> 32);
    reg_write(REG_TDLEN, TX_COUNT * sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_ENABLE | TCTL_PAD_SHORT | TCTL_COLLISION_THRESHOLD | TCTL_COLLISION_DISTANCE);
    reg_write(REG_TIPG, TIPG_DEFAULT);

    // The line needs ACPI's routing tables to be sure of, so the timer also checks for
    // arrivals in case this isn't the right one.
    if (!pci_enable_msi(card, VECTOR_NET))
        irq_enable_line(pci_read(card, PCI_INTERRUPT) & 0xFF, VECTOR_NET);
    reg_write(REG_IMS, INT_LINK_CHANGE | INT_RX_LOW | INT_RX_OVERRUN | INT_RX_TIMER);

    kprintf("e1000: %x:%x:%x:%x:%x:%x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

void e1000_mac(uint8_t out[6]) {
    memcpy(out, mac, 6);
}

void e1000_send(const void *frame, size_t len) {
    volatile struct tx_desc *desc = &tx_ring[tx_next];

    if (!(desc->status & DESC_DONE) || len > BUFFER_SIZE)
        return;

    memcpy(tx_buffers[tx_next], frame, len);
    desc->length = len;
    desc->status = 0;
    desc->command = DESC_END_OF_PACKET | TX_INSERT_CRC | TX_REPORT_STATUS;
    tx_next = (tx_next + 1) % TX_COUNT;
    reg_write(REG_TDT, tx_next);
}

void e1000_poll(void) {
    if (!registers)
        return;

    // Reading this clears it, and a level interrupt would otherwise stay raised.
    reg_read(REG_ICR);

    while (rx_ring[rx_next].status & DESC_DONE) {
        volatile struct rx_desc *desc = &rx_ring[rx_next];

        if ((desc->status & RX_END_OF_PACKET) && !desc->errors)
            net_receive(rx_buffers[rx_next], desc->length);

        desc->status = 0;
        reg_write(REG_RDT, rx_next);
        rx_next = (rx_next + 1) % RX_COUNT;
    }
}
