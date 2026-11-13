#include <errno_values.h>
#include <io.h>
#include <key_event.h>
#include <keyboard.h>
#include <mouse.h>
#include <process.h>
#include <signal_numbers.h>
#include <string.h>
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
#define CMD_DISABLE_PORT1 0xAD
#define CMD_ENABLE_PORT1 0xAE
#define CMD_DISABLE_PORT2 0xA7
#define CMD_ENABLE_PORT2 0xA8
#define CONFIG_IRQ1 0x01
#define CONFIG_IRQ12 0x02
#define CONFIG_PORT1_DISABLED 0x10
#define CONFIG_PORT2_DISABLED 0x20
#define CONFIG_TRANSLATE 0x40
#define KEYBOARD_ENABLE_SCANNING 0xF4
#define TIMEOUT 100000

#define SC_RELEASED 0x80
#define SC_EXTENDED 0xE0

#define BUFFER_SIZE 4096
#define EVENT_COUNT 256
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

// For /dev/keyboard, which takes every key while it's open.
static struct key_event events[EVENT_COUNT];
static size_t events_head;
static size_t events_count;
static bool events_open;

static uint8_t modifiers;
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

static void command(uint8_t byte) {
    wait_input_empty();
    outb(PS2_COMMAND, byte);
}

// Sets up the controller for both devices. Firmware usually leaves the keyboard
// ready, but UEFI doesn't have to. Both ports are off while the config is read,
// so a key pressed during boot can't be taken for it.
void keyboard_init(void) {
    command(CMD_DISABLE_PORT1);
    command(CMD_DISABLE_PORT2);
    flush_output();

    command(CMD_READ_CONFIG);
    wait_output_full();
    uint8_t config = inb(PS2_DATA);

    config |= CONFIG_IRQ1 | CONFIG_IRQ12 | CONFIG_TRANSLATE;
    config &= ~(CONFIG_PORT1_DISABLED | CONFIG_PORT2_DISABLED);
    command(CMD_WRITE_CONFIG);
    wait_input_empty();
    outb(PS2_DATA, config);

    command(CMD_ENABLE_PORT1);
    command(CMD_ENABLE_PORT2);
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

static void update_modifiers(uint16_t code, bool pressed) {
    uint8_t bit = 0;

    switch (code) {
    case KEY_LEFT_SHIFT:
    case KEY_RIGHT_SHIFT:
        bit = KEY_MOD_SHIFT;
        break;
    case KEY_LEFT_CTRL:
    case KEY_RIGHT_CTRL:
        bit = KEY_MOD_CTRL;
        break;
    case KEY_LEFT_ALT:
    case KEY_RIGHT_ALT:
        bit = KEY_MOD_ALT;
        break;
    case KEY_LEFT_SUPER:
    case KEY_RIGHT_SUPER:
        bit = KEY_MOD_SUPER;
        break;
    case KEY_CAPS_LOCK:
        if (pressed)
            modifiers ^= KEY_MOD_CAPS_LOCK;
        return;
    }
    modifiers = pressed ? modifiers | bit : modifiers & ~bit;
}

// The character a key types with the modifiers held now, or 0.
static char key_text(uint16_t code) {
    if (code == KEY_EXTENDED + KEY_ENTER)
        return '\r';
    if (code == (KEY_EXTENDED | 0x35))
        return '/';
    if (code >= sizeof(keymap))
        return 0;

    char c = modifiers & KEY_MOD_SHIFT ? keymap_shift[code] : keymap[code];
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

    if (letter && (modifiers & KEY_MOD_CAPS_LOCK))
        c ^= 0x20;
    if (letter && (modifiers & KEY_MOD_CTRL))
        c &= 0x1F;
    return c;
}

// What the console gets for keys that type nothing themselves.
static const char *key_sequence(uint16_t code) {
    switch (code) {
    case KEY_UP:
        return "\x1b[A";
    case KEY_DOWN:
        return "\x1b[B";
    case KEY_RIGHT:
        return "\x1b[C";
    case KEY_LEFT:
        return "\x1b[D";
    case KEY_HOME:
        return "\x1b[H";
    case KEY_END:
        return "\x1b[F";
    case KEY_DELETE:
        return "\x1b[3~";
    }
    return NULL;
}

static void to_console(uint16_t code) {
    const char *sequence = key_sequence(code);
    char c = key_text(code);

    if (sequence) {
        push(sequence, strlen(sequence));
        return;
    }
    if (c == 0)
        return;

    // With nothing in the foreground, Ctrl+C reaches the shell like any other key.
    struct tty *console = tty_console();

    if (c == CTRL_C && tty_signals_enabled(console) && process_signal_group(tty_foreground(console), SIGINT)) {
        tty_echo_interrupt(console);
        return;
    }
    push(&c, 1);
}

void keyboard_handle_byte(uint8_t byte) {
    if (byte == SC_EXTENDED) {
        extended = true;
        return;
    }

    uint16_t code = (byte & ~SC_RELEASED) | (extended ? KEY_EXTENDED : 0);
    bool pressed = !(byte & SC_RELEASED);

    extended = false;
    // Some keys come wrapped in a fake shift, which isn't a key at all.
    if (code == (KEY_EXTENDED | KEY_LEFT_SHIFT) || code == (KEY_EXTENDED | KEY_RIGHT_SHIFT))
        return;

    update_modifiers(code, pressed);
    if (events_open) {
        if (events_count < EVENT_COUNT) {
            events[(events_head + events_count++) % EVENT_COUNT] = (struct key_event){
                .code = code, .pressed = pressed, .modifiers = modifiers, .text = pressed ? key_text(code) : 0,
            };
            process_wakeup(events);
        }
    } else if (pressed) {
        to_console(code);
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
            keyboard_handle_byte(byte);
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

int keyboard_open_events(void) {
    if (events_open)
        return -EBUSY;
    events_open = true;
    events_count = 0;
    return 0;
}

void keyboard_close_events(void) {
    events_open = false;
}

bool keyboard_events_readable(void) {
    return events_count > 0;
}

int64_t keyboard_read_events(void *buf, size_t len, bool nonblocking) {
    size_t capacity = len / sizeof(struct key_event);

    if (capacity == 0)
        return -EINVAL;

    while (events_count == 0) {
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(events))
            return -EINTR;
    }

    struct key_event *out = buf;
    size_t copied = 0;

    while (copied < capacity && events_count > 0) {
        out[copied++] = events[events_head];
        events_head = (events_head + 1) % EVENT_COUNT;
        events_count--;
    }
    return copied * sizeof(struct key_event);
}
