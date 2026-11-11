#include <io.h>
#include <keyboard.h>
#include <mouse.h>
#include <process.h>
#include <tty.h>
#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64
#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define STATUS_FROM_MOUSE 0x20

#define CMD_READ_CONFIG 0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_ENABLE_PORT1 0xAE
#define CONFIG_IRQ1 0x01
#define CONFIG_PORT1_DISABLED 0x10
#define CONFIG_TRANSLATE 0x40
#define KEYBOARD_ENABLE_SCANNING 0xF4
#define TIMEOUT 100000

#define SC_RELEASED 0x80
#define SC_EXTENDED 0xE0
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL 0x1D
#define SC_CAPS 0x3A

#define BUFFER_SIZE 4096
#define CTRL_C 0x03

// Scancode set 1. The keypad acts as if num lock were always on.
static const char keymap[] = {
    0,    0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7F, '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c',  'v',
    'b',  'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,    0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',  '1',
    '2',  '3', '0', '.',
};

static const char keymap_shift[] = {
    0,    0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x7F, '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0,  'A',  'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C',  'V',
    'B',  'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,    0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+',  '1',
    '2',  '3', '0', '.',
};

static char buffer[BUFFER_SIZE];
static size_t head;
static size_t count;
static bool shift;
static bool ctrl;
static bool caps;
static bool extended;

static void wait_input_empty(void) {
    for (int i = 0; i < TIMEOUT && (inb(PS2_STATUS) & STATUS_INPUT_FULL); i++)
        ;
}

static void wait_output_full(void) {
    for (int i = 0; i < TIMEOUT && !(inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++)
        ;
}

static void flush_output(void) {
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
        inb(PS2_DATA);
}

// Firmware usually leaves the keyboard ready, but UEFI doesn't have to.
void keyboard_init(void) {
    flush_output();

    wait_input_empty();
    outb(PS2_COMMAND, CMD_READ_CONFIG);
    wait_output_full();
    uint8_t config = inb(PS2_DATA);

    config |= CONFIG_IRQ1 | CONFIG_TRANSLATE;
    config &= ~CONFIG_PORT1_DISABLED;

    wait_input_empty();
    outb(PS2_COMMAND, CMD_WRITE_CONFIG);
    wait_input_empty();
    outb(PS2_DATA, config);

    wait_input_empty();
    outb(PS2_COMMAND, CMD_ENABLE_PORT1);
    wait_input_empty();
    outb(PS2_DATA, KEYBOARD_ENABLE_SCANNING);
    wait_output_full();
    flush_output();
}

void *keyboard_channel(void) {
    return buffer;
}

// Sequences go in whole or not at all, so a full buffer can't split one.
static void push(const char *bytes, size_t len) {
    if (BUFFER_SIZE - count < len)
        return;

    for (size_t i = 0; i < len; i++)
        buffer[(head + count + i) % BUFFER_SIZE] = bytes[i];
    count += len;
    process_wakeup(buffer);
}

static void handle_extended(uint8_t scancode) {
    switch (scancode) {
    case SC_CTRL:
        ctrl = true;
        break;
    case SC_CTRL | SC_RELEASED:
        ctrl = false;
        break;
    case 0x48:
        push("\x1b[A", 3);
        break;
    case 0x50:
        push("\x1b[B", 3);
        break;
    case 0x4D:
        push("\x1b[C", 3);
        break;
    case 0x4B:
        push("\x1b[D", 3);
        break;
    case 0x47:
        push("\x1b[H", 3);
        break;
    case 0x4F:
        push("\x1b[F", 3);
        break;
    case 0x53:
        push("\x1b[3~", 4);
        break;
    case 0x1C:
        push("\r", 1);
        break;
    case 0x35:
        push("/", 1);
        break;
    }
}

static void handle_key(uint8_t scancode) {
    if (scancode >= sizeof(keymap))
        return;

    char c = shift ? keymap_shift[scancode] : keymap[scancode];
    if (c == 0)
        return;

    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c &= 0x1F;

    // With nothing in the foreground, Ctrl+C reaches the shell like any other key.
    if (c == CTRL_C && tty_signals_enabled() && process_interrupt_foreground()) {
        tty_echo_interrupt();
        return;
    }
    push(&c, 1);
}

static void handle_scancode(uint8_t scancode) {
    if (scancode == SC_EXTENDED) {
        extended = true;
        return;
    }
    if (extended) {
        extended = false;
        handle_extended(scancode);
        return;
    }

    switch (scancode) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        shift = true;
        break;
    case SC_LSHIFT | SC_RELEASED:
    case SC_RSHIFT | SC_RELEASED:
        shift = false;
        break;
    case SC_CTRL:
        ctrl = true;
        break;
    case SC_CTRL | SC_RELEASED:
        ctrl = false;
        break;
    case SC_CAPS:
        caps = !caps;
        break;
    default:
        if (!(scancode & SC_RELEASED))
            handle_key(scancode);
    }
}

// The keyboard and mouse share a controller and one data port, and the status
// register says whose byte is waiting. Either interrupt can find either's data.
void keyboard_handle_irq(void) {
    uint8_t status;

    while ((status = inb(PS2_STATUS)) & STATUS_OUTPUT_FULL) {
        uint8_t byte = inb(PS2_DATA);

        if (status & STATUS_FROM_MOUSE)
            mouse_handle_byte(byte);
        else
            handle_scancode(byte);
    }
}

size_t keyboard_read(char *buf, size_t len) {
    size_t copied = 0;

    while (copied < len && count > 0) {
        buf[copied++] = buffer[head];
        head = (head + 1) % BUFFER_SIZE;
        count--;
    }
    return copied;
}
