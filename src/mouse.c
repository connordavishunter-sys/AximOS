#include <errno_values.h>
#include <io.h>
#include <keyboard.h>
#include <mouse.h>
#include <mouse_event.h>
#include <process.h>
#include <serial.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64
#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define STATUS_FROM_MOUSE 0x20
#define TIMEOUT 1000000

#define CMD_SEND_TO_PORT2 0xD4

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

// VMware's mouse, which QEMU has too, is read through a port with a magic number.
#define VMWARE_MAGIC 0x564D5868
#define VMWARE_PORT 0x5658
#define VMWARE_GET_VERSION 10
#define VMMOUSE_DATA 39
#define VMMOUSE_STATUS 40
#define VMMOUSE_COMMAND 41
#define VMMOUSE_ENABLE 0x45414552
#define VMMOUSE_REQUEST_ABSOLUTE 0x53424152
#define VMMOUSE_VERSION 0x3442554A
#define VMMOUSE_ERROR 0xFFFF0000
#define VMMOUSE_QUEUED 0xFFFF
#define VMMOUSE_RELATIVE 0x00010000
#define VMMOUSE_LEFT 0x20
#define VMMOUSE_RIGHT 0x10
#define VMMOUSE_MIDDLE 0x08
#define CPUID_HYPERVISOR (1u << 31)

#define MAX_PACKET 4
#define BUFFER_EVENTS 256

static struct mouse_event events[BUFFER_EVENTS];
static size_t head;
static size_t count;
static uint8_t packet[MAX_PACKET];
static int packet_used;
static int packet_size = 3;
static bool vmmouse;

static void wait_input_empty(void) {
    for (int i = 0; i < TIMEOUT && (inb(PS2_STATUS) & STATUS_INPUT_FULL); i++)
        ;
}

// The mouse's next reply. Keys pressed meanwhile go to the keyboard, rather than
// being taken for replies, which would put every reply after them out by one.
static bool read_response(uint8_t *byte) {
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t status = inb(PS2_STATUS);

        if (!(status & STATUS_OUTPUT_FULL))
            continue;
        *byte = inb(PS2_DATA);
        if (status & STATUS_FROM_MOUSE)
            return true;
        keyboard_handle_byte(*byte);
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

struct backdoor {
    uint32_t a, b, c, d;
};

static struct backdoor backdoor(uint32_t command, uint32_t argument) {
    struct backdoor r = {VMWARE_MAGIC, argument, command, VMWARE_PORT};

    __asm__ volatile("inl %%dx, %%eax" : "+a"(r.a), "+b"(r.b), "+c"(r.c), "+d"(r.d));
    return r;
}

static bool under_hypervisor(void) {
    uint32_t a = 1, b, c = 0, d;

    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return c & CPUID_HYPERVISOR;
}

// Asks a virtual machine's mouse for where the host's pointer is. The port is
// only tried under a hypervisor, and the magic number only comes back from one.
static bool try_enable_vmmouse(void) {
    if (!under_hypervisor() || backdoor(VMWARE_GET_VERSION, ~VMWARE_MAGIC).b != VMWARE_MAGIC)
        return false;
    backdoor(VMMOUSE_COMMAND, VMMOUSE_ENABLE);

    uint32_t status = backdoor(VMMOUSE_STATUS, 0).a;

    if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR || !(status & VMMOUSE_QUEUED)
        || backdoor(VMMOUSE_DATA, 1).a != VMMOUSE_VERSION)
        return false;
    backdoor(VMMOUSE_COMMAND, VMMOUSE_REQUEST_ABSOLUTE);
    return true;
}

bool mouse_init(void) {
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

    vmmouse = try_enable_vmmouse();
    kprintf("mouse: found, %s%s\n", packet_size == 4 ? "with a wheel" : "no wheel",
            vmmouse ? ", placed by the virtual machine" : "");
    return true;
}

static void add_event(struct mouse_event e) {
    if (count == BUFFER_EVENTS)
        return;
    events[(head + count) % BUFFER_EVENTS] = e;
    count++;
    process_wakeup(events);
}

// The virtual machine's mouse only nudges the PS/2 one to say it has news.
static void read_vmmouse(void) {
    uint32_t status;

    while (((status = backdoor(VMMOUSE_STATUS, 0).a) & VMMOUSE_ERROR) != VMMOUSE_ERROR
           && (status & VMMOUSE_QUEUED) >= 4) {
        struct backdoor r = backdoor(VMMOUSE_DATA, 4);
        bool relative = r.a & VMMOUSE_RELATIVE;
        uint32_t buttons = (r.a & VMMOUSE_LEFT ? MOUSE_BUTTON_LEFT : 0) | (r.a & VMMOUSE_RIGHT ? MOUSE_BUTTON_RIGHT : 0)
            | (r.a & VMMOUSE_MIDDLE ? MOUSE_BUTTON_MIDDLE : 0);

        add_event((struct mouse_event){
            .dx = relative ? (int32_t)r.b : 0,
            .dy = relative ? (int32_t)r.c : 0,
            .wheel = (int8_t)r.d,
            .buttons = buttons,
            .absolute = !relative,
            .x = relative ? 0 : (int32_t)r.b,
            .y = relative ? 0 : (int32_t)r.c,
        });
    }
}

static void finish_packet(void) {
    uint8_t flags = packet[0];

    if (vmmouse) {
        read_vmmouse();
        return;
    }

    // An overflowed movement is meaningless, so only the buttons can be trusted.
    int dx = flags & PACKET_X_OVERFLOW ? 0 : packet[1] - (flags & PACKET_X_NEGATIVE ? 256 : 0);
    int dy = flags & PACKET_Y_OVERFLOW ? 0 : packet[2] - (flags & PACKET_Y_NEGATIVE ? 256 : 0);

    add_event((struct mouse_event){
        .dx = dx,
        .dy = -dy,
        .wheel = packet_size == 4 ? (int8_t)packet[3] : 0,
        .buttons = flags & PACKET_BUTTONS,
    });
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
