#include <errno_values.h>
#include <io.h>
#include <mouse.h>
#include <mouse_event.h>
#include <process.h>
#include <serial.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64
#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define TIMEOUT 1000000

#define CMD_READ_CONFIG 0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_ENABLE_PORT2 0xA8
#define CMD_SEND_TO_PORT2 0xD4
#define CONFIG_IRQ12 0x02
#define CONFIG_PORT2_DISABLED 0x20

#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_SET_SAMPLE_RATE 0xF3
#define MOUSE_GET_ID 0xF2
#define MOUSE_ENABLE_REPORTING 0xF4
#define MOUSE_RESET 0xFF
#define MOUSE_ACK 0xFA
#define MOUSE_SELF_TEST_PASSED 0xAA
#define MOUSE_ID_WHEEL 3

#define PACKET_ALWAYS_SET 0x08
#define PACKET_X_NEGATIVE 0x10
#define PACKET_Y_NEGATIVE 0x20
#define PACKET_X_OVERFLOW 0x40
#define PACKET_Y_OVERFLOW 0x80
#define PACKET_BUTTONS 0x07

#define MAX_PACKET 4
#define BUFFER_EVENTS 256

static struct mouse_event events[BUFFER_EVENTS];
static size_t head;
static size_t count;
static uint8_t packet[MAX_PACKET];
static int packet_used;
static int packet_size = 3;

static void wait_input_empty(void) {
    for (int i = 0; i < TIMEOUT && (inb(PS2_STATUS) & STATUS_INPUT_FULL); i++)
        ;
}

static bool read_response(uint8_t *byte) {
    for (int i = 0; i < TIMEOUT; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            *byte = inb(PS2_DATA);
            return true;
        }
    }
    return false;
}

static bool send(uint8_t byte) {
    uint8_t reply;

    wait_input_empty();
    outb(PS2_COMMAND, CMD_SEND_TO_PORT2);
    wait_input_empty();
    outb(PS2_DATA, byte);
    return read_response(&reply) && reply == MOUSE_ACK;
}

// A mouse with a wheel keeps quiet about it until it gets this secret knock of
// sample rates, then reports a different ID and sends four bytes per packet.
static void try_enable_wheel(void) {
    static const uint8_t knock[] = {200, 100, 80};
    uint8_t id;

    for (size_t i = 0; i < sizeof(knock); i++) {
        if (!send(MOUSE_SET_SAMPLE_RATE) || !send(knock[i]))
            return;
    }
    if (send(MOUSE_GET_ID) && read_response(&id) && id == MOUSE_ID_WHEEL)
        packet_size = 4;
}

bool mouse_init(void) {
    wait_input_empty();
    outb(PS2_COMMAND, CMD_ENABLE_PORT2);

    wait_input_empty();
    outb(PS2_COMMAND, CMD_READ_CONFIG);

    uint8_t config;
    if (!read_response(&config))
        return false;

    config |= CONFIG_IRQ12;
    config &= ~CONFIG_PORT2_DISABLED;

    wait_input_empty();
    outb(PS2_COMMAND, CMD_WRITE_CONFIG);
    wait_input_empty();
    outb(PS2_DATA, config);

    uint8_t test, id;
    if (!send(MOUSE_RESET) || !read_response(&test) || test != MOUSE_SELF_TEST_PASSED
        || !read_response(&id)) {
        kprintf("mouse: none found\n");
        return false;
    }

    try_enable_wheel();

    if (!send(MOUSE_SET_DEFAULTS) || !send(MOUSE_ENABLE_REPORTING)) {
        kprintf("mouse: didn't respond\n");
        return false;
    }

    kprintf("mouse: found, %s\n", packet_size == 4 ? "with a wheel" : "no wheel");
    return true;
}

static void finish_packet(void) {
    uint8_t flags = packet[0];

    // An overflowed movement is meaningless, so only the buttons can be trusted.
    int dx = flags & PACKET_X_OVERFLOW ? 0 : packet[1] - (flags & PACKET_X_NEGATIVE ? 256 : 0);
    int dy = flags & PACKET_Y_OVERFLOW ? 0 : packet[2] - (flags & PACKET_Y_NEGATIVE ? 256 : 0);
    int wheel = packet_size == 4 ? (int8_t)packet[3] : 0;

    if (count == BUFFER_EVENTS)
        return;

    events[(head + count) % BUFFER_EVENTS] = (struct mouse_event){
        .dx = dx,
        .dy = -dy,
        .wheel = wheel,
        .buttons = flags & PACKET_BUTTONS,
    };
    count++;
    process_wakeup(events);
}

void mouse_handle_byte(uint8_t byte) {
    // Every packet's first byte has this bit set, which is how to find the start
    // of one again after losing a byte.
    if (packet_used == 0 && !(byte & PACKET_ALWAYS_SET))
        return;

    packet[packet_used++] = byte;
    if (packet_used == packet_size) {
        packet_used = 0;
        finish_packet();
    }
}

bool mouse_readable(void) {
    return count > 0;
}

int64_t mouse_read(void *buf, size_t len, bool nonblocking) {
    size_t capacity = len / sizeof(struct mouse_event);

    if (capacity == 0)
        return -EINVAL;

    while (count == 0) {
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(events))
            return -EINTR;
    }

    struct mouse_event *out = buf;
    size_t copied = 0;

    while (copied < capacity && count > 0) {
        out[copied++] = events[head];
        head = (head + 1) % BUFFER_EVENTS;
        count--;
    }
    return copied * sizeof(struct mouse_event);
}
