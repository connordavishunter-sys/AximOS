#include <io.h>
#include <serial.h>
#include <stdarg.h>

#define COM1 0x3F8

void serial_write(const char *buf, size_t len) {
    while (len--)
        outb(COM1, *buf++);
}

static void put_string(const char *s) {
    for (; *s; s++)
        outb(COM1, *s);
}

static void put_number(uint64_t value, unsigned base) {
    char digits[21];
    size_t i = sizeof(digits) - 1;

    digits[i] = '\0';
    do {
        digits[--i] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    put_string(&digits[i]);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            outb(COM1, *fmt);
            continue;
        }

        bool is_long = *++fmt == 'l';
        if (is_long)
            fmt++;

        switch (*fmt) {
        case 's':
            put_string(va_arg(args, const char *));
            break;
        case 'c':
            outb(COM1, va_arg(args, int));
            break;
        case 'u':
            put_number(is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 10);
            break;
        case 'x':
            put_number(is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 16);
            break;
        }
    }

    va_end(args);
}
