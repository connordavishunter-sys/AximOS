#include <key_event.h>
#include <key_usage.h>
#include <keyboard.h>
#include <kmalloc.h>
#include <mouse.h>
#include <serial.h>
#include <string.h>
#include <usb.h>
#include <vfs.h>

// USB keyboards and mice, through the boot protocol every one of them has: fixed
// reports, so there's no report descriptor to make sense of. Keyboards say which
// keys are held, and the changes become the same KEY_ codes a PS/2 keyboard gives.

#define DESCRIPTOR_INTERFACE 4
#define DESCRIPTOR_ENDPOINT 5
#define CLASS_HID 3
#define SUBCLASS_BOOT 1
#define PROTOCOL_KEYBOARD 1
#define PROTOCOL_MOUSE 2
#define ENDPOINT_INTERRUPT 3

#define SET_CONFIGURATION 9
#define SET_IDLE 0x0A
#define SET_PROTOCOL 0x0B
#define CLASS_REQUEST 0x21 // to the interface
#define BOOT_PROTOCOL 0

#define MAX_INTERFACES 4
#define KEYS 6
#define ROLLOVER 0x01 // in every key's place when too many are held

// The modifier byte's bits, in order.
static const uint16_t modifier_keys[8] = {
    KEY_LEFT_CTRL, KEY_LEFT_SHIFT, KEY_LEFT_ALT, KEY_LEFT_SUPER,
    KEY_RIGHT_CTRL, KEY_RIGHT_SHIFT, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,
};

struct keyboard {
    uint8_t modifiers;
    uint8_t keys[KEYS];
};

static void send_key(uint8_t usage, uint8_t modifiers, bool pressed) {
    uint16_t code = usage < sizeof(key_usages) / sizeof(key_usages[0]) ? key_usages[usage] : 0;

    // As a PS/2 keyboard sends it, which is what takes a window's picture.
    if (code == KEY_PRINT_SCREEN && modifiers & 0x44)
        code = KEY_SYSRQ;
    if (code)
        keyboard_key(code, pressed);
}

static bool holds(const uint8_t *keys, uint8_t usage) {
    for (int i = 0; i < KEYS; i++) {
        if (keys[i] == usage)
            return true;
    }
    return false;
}

static void keyboard_report(void *context, const uint8_t *data, uint32_t length) {
    struct keyboard *k = context;
    const uint8_t *keys = data + 2;

    if (length < 2 + KEYS || keys[0] == ROLLOVER)
        return;
    for (int i = 0; i < 8; i++) {
        if ((k->modifiers ^ data[0]) & 1 << i)
            keyboard_key(modifier_keys[i], data[0] & 1 << i);
    }
    for (int i = 0; i < KEYS; i++) {
        if (k->keys[i] && !holds(keys, k->keys[i]))
            send_key(k->keys[i], data[0], false);
    }
    for (int i = 0; i < KEYS; i++) {
        if (keys[i] && !holds(k->keys, keys[i]))
            send_key(keys[i], data[0], true);
    }
    k->modifiers = data[0];
    memcpy(k->keys, keys, KEYS);
}

static void mouse_report(void *context, const uint8_t *data, uint32_t length) {
    (void)context;
    if (length >= 3)
        mouse_add_motion((int8_t)data[1], (int8_t)data[2], length >= 4 ? -(int8_t)data[3] : 0, data[0] & 7);
}

struct interface {
    uint8_t number, protocol, endpoint, interval;
    uint16_t packet;
};

bool usb_hid_probe(struct usb_device *usb, const uint8_t *config, uint16_t length) {
    struct interface found[MAX_INTERFACES], *current = NULL;
    int count = 0;

    for (uint16_t at = 0; at + 2 <= length && config[at] >= 2 && at + config[at] <= length; at += config[at]) {
        const uint8_t *desc = &config[at];

        if (desc[1] == DESCRIPTOR_INTERFACE && desc[0] >= 9) {
            bool boot = desc[5] == CLASS_HID && desc[6] == SUBCLASS_BOOT
                && (desc[7] == PROTOCOL_KEYBOARD || desc[7] == PROTOCOL_MOUSE);

            current = boot && count < MAX_INTERFACES ? &found[count] : NULL;
            if (current)
                *current = (struct interface){.number = desc[2], .protocol = desc[7]};
        } else if (current && desc[1] == DESCRIPTOR_ENDPOINT && desc[0] >= 7 && (desc[2] & 0x80)
                   && (desc[3] & 3) == ENDPOINT_INTERRUPT) {
            current->endpoint = desc[2];
            current->packet = (desc[4] | desc[5] << 8) & 0x7FF;
            current->interval = desc[6];
            current = NULL;
            count++;
        }
    }
    if (!count)
        return false;
    if (!usb_control(usb, 0, SET_CONFIGURATION, config[5], 0, NULL, 0)) {
        kprintf("usb: input device won't start\n");
        return true;
    }

    for (struct interface *i = found; i < found + count; i++) {
        bool keyboard = i->protocol == PROTOCOL_KEYBOARD;
        void *context = keyboard ? kmalloc(sizeof(struct keyboard)) : NULL;

        if (keyboard && !context)
            continue;
        if (context)
            memset(context, 0, sizeof(struct keyboard));
        // Without the boot protocol, a report could be laid out any way, but most
        // keyboards and mice use the same layout anyway, so it's worth trying.
        if (!usb_control(usb, CLASS_REQUEST, SET_PROTOCOL, BOOT_PROTOCOL, i->number, NULL, 0))
            kprintf("usb: interface %u keeps its own reports\n", i->number);
        usb_control(usb, CLASS_REQUEST, SET_IDLE, 0, i->number, NULL, 0); // only report changes
        if (!usb_open_interrupt(usb, i->endpoint, i->packet, i->interval, keyboard ? keyboard_report : mouse_report,
                                context)) {
            kprintf("usb: interface %u won't report\n", i->number);
            continue;
        }
        if (!keyboard)
            vfs_add_device(VFS_DEVICE_MOUSE);
        kprintf("usb: found a %s\n", keyboard ? "keyboard" : "mouse");
    }
    return true;
}
