#include <console.h>
#include <errno_values.h>
#include <irq.h>
#include <keyboard.h>
#include <process.h>
#include <string.h>
#include <tty.h>

#define LINE_SIZE 4096
#define TICKS_PER_VTIME (TIMER_HZ / 10)

static struct termios settings = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = {[VINTR] = 3, [VQUIT] = 28, [VERASE] = 0x7F, [VKILL] = 21, [VEOF] = 4, [VMIN] = 1},
};

// line[0..ready] can be read, and the rest is a line still being typed.
static char line[LINE_SIZE];
static size_t line_len;
static size_t ready;
static bool eof_pending;

static void echo(char c) {
    if (!(settings.c_lflag & ECHO) && !(c == '\n' && (settings.c_lflag & ECHONL)))
        return;

    // Control characters show as ^X, as anything else would act on the screen.
    if ((uint8_t)c < 0x20 && c != '\n' && c != '\t') {
        char shown[2] = {'^', c + '@'};

        console_write(shown, 2);
    } else {
        console_write(&c, 1);
    }
}

static void erase_last(void) {
    if (line_len == ready)
        return;

    bool control = (uint8_t)line[line_len - 1] < 0x20 && line[line_len - 1] != '\t';

    line_len--;
    if ((settings.c_lflag & ECHO) && (settings.c_lflag & ECHOE))
        console_write("\b \b\b \b", control ? 6 : 3);
}

static void handle_byte(char c) {
    uint32_t iflag = settings.c_iflag;
    const uint8_t *cc = settings.c_cc;

    if (c == '\r' && (iflag & IGNCR))
        return;
    if (c == '\r' && (iflag & ICRNL))
        c = '\n';
    else if (c == '\n' && (iflag & INLCR))
        c = '\r';

    if (settings.c_lflag & ICANON) {
        if ((uint8_t)c == cc[VERASE] || c == '\b') {
            erase_last();
            return;
        }
        if ((uint8_t)c == cc[VKILL]) {
            while (line_len > ready)
                erase_last();
            return;
        }
        if ((uint8_t)c == cc[VEOF]) {
            eof_pending = ready == line_len;
            ready = line_len;
            return;
        }
        // A full line keeps room for the newline that ends it.
        if (line_len == LINE_SIZE - 1 && c != '\n')
            return;
    }

    line[line_len++] = c;
    echo(c);
    if (!(settings.c_lflag & ICANON) || c == '\n')
        ready = line_len;
}

// Keys are worked through by whoever wants input next, not by the interrupt, so a
// program that turns echo off can't lose the race with what was typed just before.
static void take_keys(void) {
    char c;

    while (line_len < LINE_SIZE && keyboard_read(&c, 1) == 1)
        handle_byte(c);
}

static size_t bytes_wanted(size_t len) {
    if (settings.c_lflag & ICANON)
        return 1;

    size_t min = settings.c_cc[VMIN];

    return min == 0 ? 1 : min < len ? min : len;
}

int64_t tty_read(void *buf, size_t len, bool nonblocking) {
    if (len == 0)
        return 0;

    uint64_t last_change = irq_ticks();
    size_t last_ready = ready;

    for (;;) {
        take_keys();

        if (ready != last_ready) {
            last_ready = ready;
            last_change = irq_ticks();
        }

        bool timed = !(settings.c_lflag & ICANON) && settings.c_cc[VTIME] > 0;
        uint64_t limit = settings.c_cc[VTIME] * TICKS_PER_VTIME;
        uint64_t waited = irq_ticks() - last_change;
        bool can_time_out = timed && (settings.c_cc[VMIN] == 0 || ready > 0);
        bool timed_out = can_time_out && waited >= limit;
        bool poll_only = !(settings.c_lflag & ICANON) && settings.c_cc[VMIN] == 0 && !timed;

        if (ready >= bytes_wanted(len) || (ready > 0 && nonblocking) || poll_only || timed_out) {
            size_t count = ready < len ? ready : len;

            memcpy(buf, line, count);
            memmove(line, line + count, line_len - count);
            line_len -= count;
            ready -= count;
            return count;
        }
        if (eof_pending) {
            eof_pending = false;
            return 0;
        }
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep_timeout(keyboard_channel(), can_time_out ? limit - waited : 0))
            return -EINTR;
    }
}

bool tty_readable(void) {
    take_keys();

    bool polling = !(settings.c_lflag & ICANON) && settings.c_cc[VMIN] == 0;

    return ready > 0 || eof_pending || polling;
}

size_t tty_pending(void) {
    take_keys();
    return ready;
}

void tty_get_settings(struct termios *out) {
    *out = settings;
}

void tty_set_settings(const struct termios *new_settings, bool discard_input) {
    bool was_canonical = settings.c_lflag & ICANON;

    if (discard_input) {
        char c;

        while (keyboard_read(&c, 1) == 1)
            ;
        line_len = ready = 0;
        eof_pending = false;
    }

    settings = *new_settings;

    // A half typed line can be read once nothing is waiting for its end.
    if (was_canonical && !(settings.c_lflag & ICANON))
        ready = line_len;
}

void tty_echo_interrupt(void) {
    if (settings.c_lflag & ECHO)
        console_write("^C", 2);
}

bool tty_signals_enabled(void) {
    return settings.c_lflag & ISIG;
}
