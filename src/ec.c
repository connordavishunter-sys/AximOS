#include <acpi.h>
#include <clock.h>
#include <io.h>
#include <serial.h>
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

// The embedded controller: the laptop's own small computer, which looks after the
// battery, the fans and the keyboard's special keys. The firmware's code reads and
// writes its 256 bytes of registers through two I/O ports, a byte at a time, and it
// raises a GPE when it has news, which a query turns into one of the firmware's _Qxx
// methods to run.

#define STATUS_OUTPUT_FULL 0x01 // a byte waits in the data port
#define STATUS_INPUT_FULL 0x02  // the EC hasn't taken the last byte yet
#define COMMAND_READ 0x80
#define COMMAND_WRITE 0x81
#define COMMAND_QUERY 0x84
#define WAIT_NS (50 * 1000000ULL) // far longer than any EC takes
#define HID_EC "PNP0C09"

static uint16_t data_port, command_port;
static uacpi_namespace_node *ec;
static bool global_lock;
static uint16_t gpe;
static uacpi_handle query_work;
static bool query_pending;

static bool wait_status(uint8_t mask, uint8_t want) {
    for (uint64_t until = clock_monotonic_ns() + WAIT_NS; clock_monotonic_ns() < until;) {
        if ((inb(command_port) & mask) == want)
            return true;
    }
    return false;
}

static bool send(uint16_t port, uint8_t byte) {
    if (!wait_status(STATUS_INPUT_FULL, 0))
        return false;
    outb(port, byte);
    return true;
}

static bool receive(uint8_t *byte) {
    if (!wait_status(STATUS_OUTPUT_FULL, STATUS_OUTPUT_FULL))
        return false;
    *byte = inb(data_port);
    return true;
}

// One read, write or query, each command's bytes in turn, with the firmware's global
// lock if the EC asks for it.
static bool transaction(uint8_t command, uint8_t address, uint8_t *value) {
    uacpi_u32 seq = 0;
    bool locked = global_lock && uacpi_acquire_global_lock(0xFFFF, &seq) == UACPI_STATUS_OK;
    bool ok = send(command_port, command)
        && (command == COMMAND_QUERY ? receive(value)
            : command == COMMAND_READ ? send(data_port, address) && receive(value)
                                      : send(data_port, address) && send(data_port, *value) && wait_status(STATUS_INPUT_FULL, 0));

    if (locked)
        uacpi_release_global_lock(seq);
    if (!ok)
        kprintf("ec: command %x at %x timed out\n", command, address);
    return ok;
}

// What the firmware's code reads and writes, which can be several bytes at once.
static uacpi_status region(uacpi_region_op op, uacpi_handle data) {
    uacpi_region_rw_data *rw = data;

    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE)
        return UACPI_STATUS_OK;
    if (op == UACPI_REGION_OP_READ)
        rw->value = 0;
    for (uint8_t i = 0; i < rw->byte_width; i++) {
        uint8_t byte = op == UACPI_REGION_OP_WRITE ? rw->value >> i * 8 : 0;

        if (!transaction(op == UACPI_REGION_OP_READ ? COMMAND_READ : COMMAND_WRITE, rw->offset + i, &byte))
            return UACPI_STATUS_HARDWARE_TIMEOUT;
        if (op == UACPI_REGION_OP_READ)
            rw->value |= (uint64_t)byte << i * 8;
    }
    return UACPI_STATUS_OK;
}

// Asks what happened until there's nothing left, running the method for each, then
// lets the GPE fire again. This runs as deferred work, as the methods can sleep.
static void run_queries(uacpi_handle unused) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t query;

    (void)unused;
    query_pending = false;
    while (transaction(COMMAND_QUERY, 0, &query) && query) {
        char method[] = {'_', 'Q', hex[query >> 4], hex[query & 0xF], 0};

        uacpi_eval_simple(ec, method, NULL);
    }
    uacpi_finish_handling_gpe(NULL, gpe);
}

static uacpi_interrupt_ret gpe_fired(uacpi_handle unused, uacpi_namespace_node *device, uacpi_u16 index) {
    (void)unused, (void)device, (void)index;
    if (!query_pending) {
        query_pending = true;
        uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, query_work, run_queries, NULL);
    }
    return UACPI_INTERRUPT_HANDLED;
}

// Its data port comes first in its resources, then its command and status port.
static uacpi_iteration_decision found_port(void *unused, uacpi_resource *r) {
    uint16_t port = r->type == UACPI_RESOURCE_TYPE_IO ? r->io.minimum
                  : r->type == UACPI_RESOURCE_TYPE_FIXED_IO ? r->fixed_io.address : 0;

    (void)unused;
    if (port && !data_port)
        data_port = port;
    else if (port)
        command_port = port;
    return command_port ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_ec(void *unused, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)unused, (void)depth;
    data_port = command_port = 0;
    uacpi_for_each_device_resource(node, "_CRS", found_port, NULL);
    if (command_port)
        ec = node;
    return ec ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

void ec_init(void) {
    uacpi_u64 value;

    uacpi_find_devices(HID_EC, found_ec, NULL);
    if (!ec)
        return;
    global_lock = uacpi_eval_simple_integer(ec, "_GLK", &value) == UACPI_STATUS_OK && value;
    if (uacpi_install_address_space_handler(ec, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, region, NULL)
        != UACPI_STATUS_OK) {
        kprintf("ec: the firmware won't let it be used\n");
        ec = NULL;
        return;
    }
    kprintf("ec: ports %x and %x\n", data_port, command_port);
}

// Once the firmware's GPEs are set up, its own goes to the queries.
void ec_start_events(void) {
    uacpi_u64 value;

    if (!ec || uacpi_eval_simple_integer(ec, "_GPE", &value) != UACPI_STATUS_OK)
        return;
    gpe = value;
    query_work = uacpi_kernel_create_work_item();
    if (query_work && uacpi_install_gpe_handler(NULL, gpe, UACPI_GPE_TRIGGERING_EDGE, gpe_fired, NULL) == UACPI_STATUS_OK
        && uacpi_enable_gpe(NULL, gpe) == UACPI_STATUS_OK)
        kprintf("ec: events on GPE %x\n", gpe);
}
