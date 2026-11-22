#include <acpi.h>
#include <io.h>
#include <limine.h>
#include <mm.h>
#include <process.h>
#include <serial.h>
#include <string.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define ISA_IRQ_COUNT 16

#define MADT_IO_APIC 1
#define MADT_ISA_OVERRIDE 2
#define MADT_LOCAL_APIC_OVERRIDE 5

#define OVERRIDE_POLARITY_MASK 0x3
#define OVERRIDE_POLARITY_LOW 0x3
#define OVERRIDE_TRIGGER_MASK 0xC
#define OVERRIDE_TRIGGER_LEVEL 0xC

#define SLEEP_TYPE_SHIFT 10
#define SLEEP_ENABLE (1 << 13)
#define PM1_SCI_ENABLE 1
#define FADT_POWER_BUTTON_IS_DEVICE (1 << 4) // rather than the fixed one
#define ACPI_NOTIFY_STATUS 0x80 // what a button or the lid sends when pressed or moved
#define ACPI_HID_POWER_BUTTON "PNP0C0C"
#define ACPI_HID_LID "PNP0C0D"
#define FADT_RESET_SUPPORTED (1 << 10)
#define ADDRESS_SPACE_MEMORY 0
#define ADDRESS_SPACE_IO 1
#define ACPI_ENABLE_TIMEOUT 1000000

#define AML_PACKAGE 0x12
#define AML_BYTE_PREFIX 0x0A

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
} __attribute__((packed));

struct table_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct table_header header;
    uint32_t local_apic;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

struct generic_address {
    uint8_t space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct fadt {
    struct table_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios;
    uint8_t pstate;
    uint32_t pm1a_event;
    uint32_t pm1b_event;
    uint32_t pm1a_control;
    uint32_t pm1b_control;
    uint32_t pm2_control, pm_timer, gpe0, gpe1;
    uint8_t pm1_event_length, pm1_control_length, pm2_control_length, pm_timer_length;
    uint8_t gpe0_length, gpe1_length, gpe1_base, cstate_control;
    uint8_t reserved1[16];
    uint32_t flags;
    struct generic_address reset_register;
    uint8_t reset_value;
    uint8_t reserved2[3];
    uint64_t firmware_ctrl_64;
    uint64_t dsdt_64;
} __attribute__((packed));

static_assert(offsetof(struct fadt, flags) == 112 && offsetof(struct fadt, dsdt_64) == 140);

static uint64_t local_apic_address;
static uint64_t io_apic_address;
static struct acpi_isa_route isa_routes[ISA_IRQ_COUNT];
static uint16_t isa_flags[ISA_IRQ_COUNT]; // as overridden, where 0 is the bus's usual
static const struct fadt *fadt;
static uint16_t sleep_type_a;
static uint16_t sleep_type_b;
static bool can_power_off;
static uint64_t hpet_address;

struct hpet_table {
    struct table_header header;
    uint32_t block_id;
    uint8_t space, bit_width, bit_offset, access_size; // where the registers are
    uint64_t address;
} __attribute__((packed));

static const struct table_header *find_table(const struct rsdp *rsdp, const char *signature) {
    bool extended = rsdp->revision >= 2 && rsdp->xsdt != 0;
    const struct table_header *root = mm_phys_to_virt(extended ? rsdp->xsdt : rsdp->rsdt);
    size_t entry_size = extended ? 8 : 4;
    size_t count = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *entries = (const uint8_t *)(root + 1);

    for (size_t i = 0; i < count; i++) {
        uint64_t phys = 0;

        memcpy(&phys, &entries[i * entry_size], entry_size);

        const struct table_header *table = mm_phys_to_virt(phys);
        if (memcmp(table->signature, signature, 4) == 0)
            return table;
    }
    return NULL;
}

static void parse_madt(const struct madt *madt) {
    local_apic_address = madt->local_apic;

    for (int i = 0; i < ISA_IRQ_COUNT; i++)
        isa_routes[i] = (struct acpi_isa_route){.gsi = i};

    size_t length = madt->header.length - sizeof(*madt);

    for (size_t at = 0; at + 2 <= length && madt->entries[at + 1] != 0; at += madt->entries[at + 1]) {
        const uint8_t *entry = &madt->entries[at];

        if (entry[0] == MADT_IO_APIC && io_apic_address == 0) {
            uint32_t address;

            memcpy(&address, &entry[4], sizeof(address));
            io_apic_address = address;
        } else if (entry[0] == MADT_ISA_OVERRIDE && entry[3] < ISA_IRQ_COUNT) {
            uint32_t gsi;
            uint16_t flags;

            memcpy(&gsi, &entry[4], sizeof(gsi));
            memcpy(&flags, &entry[8], sizeof(flags));
            isa_flags[entry[3]] = flags;
            isa_routes[entry[3]] = (struct acpi_isa_route){
                .gsi = gsi,
                .active_low = (flags & OVERRIDE_POLARITY_MASK) == OVERRIDE_POLARITY_LOW,
                .level_triggered = (flags & OVERRIDE_TRIGGER_MASK) == OVERRIDE_TRIGGER_LEVEL,
            };
        } else if (entry[0] == MADT_LOCAL_APIC_OVERRIDE) {
            memcpy(&local_apic_address, &entry[4], sizeof(local_apic_address));
        }
    }
}

// Reads a small number from AML, which either has a byte prefix or is the value.
static uint16_t read_aml_small_number(const uint8_t **at) {
    if (**at == AML_BYTE_PREFIX)
        ++*at;
    return *(*at)++;
}

// The sleep values for "off" are in the \_S5 package in the DSDT. Finding them
// properly takes a full AML interpreter, but the package has a simple fixed form.
static void find_sleep_type(const struct table_header *dsdt) {
    const uint8_t *bytes = (const uint8_t *)dsdt;

    for (uint32_t i = sizeof(*dsdt); i + 12 < dsdt->length; i++) {
        if (memcmp(&bytes[i], "_S5_", 4) != 0 || bytes[i + 4] != AML_PACKAGE)
            continue;

        const uint8_t *at = &bytes[i + 5];

        // The package length takes 1 to 4 bytes, given by its top two bits.
        at += (*at >> 6) + 1;
        at++; // the element count

        sleep_type_a = read_aml_small_number(&at);
        sleep_type_b = read_aml_small_number(&at);
        can_power_off = true;
        return;
    }
}

static bool running; // uACPI, which runs the firmware's own code from here on
static uacpi_namespace_node *lid;

static bool step(uacpi_status status, const char *what) {
    if (status != UACPI_STATUS_OK)
        kprintf("acpi: %s failed: %s\n", what, uacpi_status_to_string(status));
    return status == UACPI_STATUS_OK;
}

bool acpi_init(void) {
    if (!rsdp_request.response) {
        kprintf("acpi: no RSDP from the bootloader\n");
        return false;
    }

    const struct rsdp *rsdp = rsdp_request.response->address;
    const struct madt *madt = (const struct madt *)find_table(rsdp, "APIC");

    if (!madt) {
        kprintf("acpi: no MADT\n");
        return false;
    }
    parse_madt(madt);

    fadt = (const struct fadt *)find_table(rsdp, "FACP");
    if (fadt) {
        bool has_64 = fadt->header.length >= offsetof(struct fadt, dsdt_64) + sizeof(uint64_t)
            && fadt->dsdt_64 != 0;

        find_sleep_type(mm_phys_to_virt(has_64 ? fadt->dsdt_64 : fadt->dsdt));
    }

    const struct hpet_table *hpet = (const struct hpet_table *)find_table(rsdp, "HPET");

    if (hpet && hpet->space == 0) // in memory
        hpet_address = hpet->address;

    kprintf("acpi: local APIC %lx, I/O APIC %lx, power off %s\n", local_apic_address,
            io_apic_address, can_power_off ? "available" : "unavailable");
    return io_apic_address != 0;
}

uint64_t acpi_local_apic_address(void) {
    return local_apic_address;
}

uint64_t acpi_io_apic_address(void) {
    return io_apic_address;
}

uint64_t acpi_hpet_address(void) {
    return hpet_address;
}

struct acpi_isa_route acpi_isa_route(int irq) {
    return isa_routes[irq];
}

void acpi_power_off(void) {
    if (running && step(uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5), "preparing to power off"))
        step(uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5), "powering off");
    if (!fadt || !can_power_off || fadt->pm1a_control == 0)
        return;

    // Some firmware leaves ACPI to be switched on by the operating system.
    if (!(inw(fadt->pm1a_control) & PM1_SCI_ENABLE) && fadt->smi_command && fadt->acpi_enable) {
        outb(fadt->smi_command, fadt->acpi_enable);
        for (int i = 0; i < ACPI_ENABLE_TIMEOUT && !(inw(fadt->pm1a_control) & PM1_SCI_ENABLE); i++)
            ;
    }

    outw(fadt->pm1a_control, sleep_type_a << SLEEP_TYPE_SHIFT | SLEEP_ENABLE);
    if (fadt->pm1b_control)
        outw(fadt->pm1b_control, sleep_type_b << SLEEP_TYPE_SHIFT | SLEEP_ENABLE);
}

void acpi_reset(void) {
    if (running)
        step(uacpi_reboot(), "restarting");
    if (!fadt || fadt->header.length < offsetof(struct fadt, firmware_ctrl_64)
        || !(fadt->flags & FADT_RESET_SUPPORTED))
        return;

    const struct generic_address *reg = &fadt->reset_register;

    if (reg->space == ADDRESS_SPACE_IO) {
        outb(reg->address, fadt->reset_value);
    } else if (reg->space == ADDRESS_SPACE_MEMORY) {
        volatile uint8_t *mapped = mm_map_device(reg->address, 1);

        if (mapped)
            *mapped = fadt->reset_value;
    }
}

uint64_t acpi_rsdp_address(void) {
    return rsdp_request.response ? mm_virt_to_phys(rsdp_request.response->address) : 0;
}

// The SCI is active low and level triggered, unless the MADT says otherwise.
void acpi_route_sci(int irq) {
    uint16_t flags = isa_flags[irq];

    isa_routes[irq].active_low = (flags & OVERRIDE_POLARITY_MASK) == 0 || (flags & OVERRIDE_POLARITY_MASK) == OVERRIDE_POLARITY_LOW;
    isa_routes[irq].level_triggered = (flags & OVERRIDE_TRIGGER_MASK) == 0 || (flags & OVERRIDE_TRIGGER_MASK) == OVERRIDE_TRIGGER_LEVEL;
}


// Either kind of power button shuts down, as the desktop's Shut down does.
static uacpi_interrupt_ret fixed_power_button(uacpi_handle unused) {
    (void)unused;
    kprintf("acpi: power button pressed\n");
    process_signal_init(SIGPWR);
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_status device_power_button(uacpi_handle unused, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)unused, (void)node;
    if (value == ACPI_NOTIFY_STATUS)
        fixed_power_button(NULL);
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_power_button(void *unused, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)unused, (void)depth;
    uacpi_install_notify_handler(node, device_power_button, NULL);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

int acpi_lid_open(void) {
    uacpi_u64 open;

    return lid && uacpi_eval_simple_integer(lid, "_LID", &open) == UACPI_STATUS_OK ? open != 0 : -1;
}

static uacpi_status lid_changed(uacpi_handle unused, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)unused, (void)node;
    if (value == ACPI_NOTIFY_STATUS)
        kprintf("acpi: lid %s\n", acpi_lid_open() ? "opened" : "closed");
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_lid(void *unused, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)unused, (void)depth;
    lid = node;
    uacpi_install_notify_handler(node, lid_changed, NULL);
    return UACPI_ITERATION_DECISION_BREAK;
}

void acpi_start(void) {
    if (!step(uacpi_initialize(0), "starting") || !step(uacpi_namespace_load(), "loading the firmware's tables")
        || !step(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC), "choosing the I/O APIC")
        || !step(uacpi_namespace_initialize(), "setting up the devices"))
        return;
    running = true;
    step(uacpi_finalize_gpe_initialization(), "turning on events");
    if (!(fadt && fadt->flags & FADT_POWER_BUTTON_IS_DEVICE))
        step(uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power_button, NULL),
             "using the power button");
    uacpi_find_devices(ACPI_HID_POWER_BUTTON, found_power_button, NULL);
    uacpi_find_devices(ACPI_HID_LID, found_lid, NULL);
    kprintf("acpi: running the firmware's code%s\n", lid ? (acpi_lid_open() ? ", lid open" : ", lid closed") : "");
}
