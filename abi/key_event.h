#pragma once

#include <stdint.h>

// Shared by the kernel and libc. While /dev/keyboard is open, every key press and
// release is read from it as one of these, and the console gets none of them.

// Codes are scancode set 1, with KEY_EXTENDED added for the keys that send 0xE0 first.
#define KEY_EXTENDED 0x100
#define KEY_ESCAPE 0x01
#define KEY_BACKSPACE 0x0E
#define KEY_TAB 0x0F
#define KEY_ENTER 0x1C
#define KEY_LEFT_CTRL 0x1D
#define KEY_LEFT_SHIFT 0x2A
#define KEY_RIGHT_SHIFT 0x36
#define KEY_LEFT_ALT 0x38
#define KEY_SPACE 0x39
#define KEY_CAPS_LOCK 0x3A
#define KEY_F1 0x3B // to KEY_F10, in order
#define KEY_F10 0x44
#define KEY_F11 0x57
#define KEY_F12 0x58
#define KEY_RIGHT_CTRL (KEY_EXTENDED | 0x1D)
#define KEY_RIGHT_ALT (KEY_EXTENDED | 0x38)
#define KEY_HOME (KEY_EXTENDED | 0x47)
#define KEY_UP (KEY_EXTENDED | 0x48)
#define KEY_PAGE_UP (KEY_EXTENDED | 0x49)
#define KEY_LEFT (KEY_EXTENDED | 0x4B)
#define KEY_RIGHT (KEY_EXTENDED | 0x4D)
#define KEY_END (KEY_EXTENDED | 0x4F)
#define KEY_DOWN (KEY_EXTENDED | 0x50)
#define KEY_PAGE_DOWN (KEY_EXTENDED | 0x51)
#define KEY_INSERT (KEY_EXTENDED | 0x52)
#define KEY_DELETE (KEY_EXTENDED | 0x53)
#define KEY_LEFT_SUPER (KEY_EXTENDED | 0x5B)
#define KEY_RIGHT_SUPER (KEY_EXTENDED | 0x5C)

#define KEY_MOD_SHIFT 0x01
#define KEY_MOD_CTRL 0x02
#define KEY_MOD_ALT 0x04
#define KEY_MOD_SUPER 0x08
#define KEY_MOD_CAPS_LOCK 0x10

struct key_event {
    uint16_t code;
    uint8_t pressed;   // 1 when pressed or repeating, 0 when released
    uint8_t modifiers; // which KEY_MOD_ are held after this event
    uint32_t text;     // what the key types, as the console would get it, or 0
};
