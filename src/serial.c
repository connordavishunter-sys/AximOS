#include <console.h>
#include <io.h>
#include <serial.h>
#include <stdarg.h>

#define COM1 0x3F8
#define LOG_SIZE (64 * 1024)
#define LINE_MAX 512 // the most of one kprintf that's kept

static char log[LOG_SIZE];
static uint64_t log_total; // every byte ever logged, so log[log_total % LOG_SIZE] is next
static bool to_screen;

void serial_write(const char *buf, size_t len) {
    while (len--)
        outb(COM1, *buf++);
}

static void emit(const char *buf, size_t len) {
    serial_write(buf, len);
    for (size_t i = 0; i < len; i++)
        log[log_total++ % LOG_SIZE] = buf[i];
    if (to_screen)
        console_draw(buf, len);
}

size_t log_size(void) {
    return log_total < LOG_SIZE ? log_total : LOG_SIZE;
}

size_t log_read(uint64_t offset, void *buf, size_t len) {
    uint64_t first = log_total - log_size();
    size_t done = 0;

    for (; done < len && offset + done < log_size(); done++)
        ((char *)buf)[done] = log[(first + offset + done) % LOG_SIZE];
    return done;
}

// Draws the log from an offset on, through a small buffer, as it wraps around.
static void draw_from(uint64_t offset) {
    char chunk[256];
    size_t got;

    while ((got = log_read(offset, chunk, sizeof(chunk))) > 0) {
        console_draw(chunk, got);
        offset += got;
    }
}

void log_to_screen(bool on) {
    if (on && !to_screen)
        draw_from(0);
    to_screen = on;
}

void log_show_crash(void) {
    int rows, cols, lines = 0;
    uint64_t start = log_size();
    char c;

    console_size(&rows, &cols);
    while (start > 0 && lines < rows - 4)
        lines += log_read(--start, &c, 1) && c == '\n';
    console_show();
    console_draw("\033[0m\033[2J\033[H", 11);
    draw_from(start + (start > 0));
    to_screen = true;
}

static size_t put(char *line, size_t at, const char *s) {
    while (*s && at < LINE_MAX)
        line[at++] = *s++;
    return at;
}

static size_t put_number(char *line, size_t at, uint64_t value, unsigned base) {
    char digits[21];
    size_t i = sizeof(digits) - 1;

    digits[i] = '\0';
    do {
        digits[--i] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    return put(line, at, &digits[i]);
}

void kprintf(const char *fmt, ...) {
    char line[LINE_MAX];
    size_t at = 0;
    va_list args;

    va_start(args, fmt);
    for (; *fmt && at < LINE_MAX; fmt++) {
        if (*fmt != '%') {
            line[at++] = *fmt;
            continue;
        }

        bool is_long = *++fmt == 'l';
        if (is_long)
            fmt++;

        switch (*fmt) {
        case 's':
            at = put(line, at, va_arg(args, const char *));
            break;
        case 'c':
            line[at++] = va_arg(args, int);
            break;
        case 'u':
            at = put_number(line, at, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 10);
            break;
        case 'x':
            at = put_number(line, at, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 16);
            break;
        }
    }
    va_end(args);
    emit(line, at);
}
