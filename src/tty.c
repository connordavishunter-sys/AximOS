#include <console.h>
#include <errno_values.h>
#include <fs_types.h>
#include <irq.h>
#include <keyboard.h>
#include <kmalloc.h>
#include <mm.h>
#include <process.h>
#include <signal_numbers.h>
#include <string.h>
#include <tty.h>

#define LINE_SIZE PAGE_SIZE
#define OUTPUT_SIZE PAGE_SIZE
#define TICKS_PER_VTIME (TIMER_HZ / 10)

struct tty {
    struct termios settings;
    int foreground; // the process group that gets its signals, or 0

    // line[0..ready] can be read, and the rest is a line still being typed.
    char *line;
    size_t line_len;
    size_t ready;
    bool eof_pending;

    // Only for pseudo-terminals. The console's keys come from the keyboard and
    // its output goes to the screen.
    bool is_pty;
    int refs;      // the master, the slaves and processes it's the terminal of
    int slaves;    // open slave files
    bool hung_up;  // the master has closed
    struct winsize size;
    char *output;  // written by programs, for the master to read
    size_t output_head;
    size_t output_count;
};

static const struct termios default_settings = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = {[VINTR] = 3, [VQUIT] = 28, [VERASE] = 0x7F, [VKILL] = 21, [VEOF] = 4, [VMIN] = 1},
};

static char console_line[LINE_SIZE];
static struct tty console = {.settings = default_settings, .line = console_line};

struct tty *tty_console(void) {
    return &console;
}

// Doesn't wait: echo and output that don't fit are lost rather than block the typing.
static size_t put_output(struct tty *tty, const char *buf, size_t len) {
    size_t done = 0;

    if (!tty->is_pty) {
        console_write(buf, len);
        return len;
    }
    for (; done < len; done++) {
        bool crlf = buf[done] == '\n' && (tty->settings.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);

        if (OUTPUT_SIZE - tty->output_count < (crlf ? 2u : 1u))
            break;
        if (crlf)
            tty->output[(tty->output_head + tty->output_count++) % OUTPUT_SIZE] = '\r';
        tty->output[(tty->output_head + tty->output_count++) % OUTPUT_SIZE] = buf[done];
    }
    if (done > 0)
        process_wakeup(&tty->output);
    return done;
}

static void echo(struct tty *tty, char c) {
    if (!(tty->settings.c_lflag & ECHO) && !(c == '\n' && (tty->settings.c_lflag & ECHONL)))
        return;

    // Control characters show as ^X, as anything else would act on the screen.
    if ((uint8_t)c < 0x20 && c != '\n' && c != '\t') {
        char shown[2] = {'^', c + '@'};

        put_output(tty, shown, 2);
    } else {
        put_output(tty, &c, 1);
    }
}

static void erase_last(struct tty *tty) {
    if (tty->line_len == tty->ready)
        return;

    bool control = (uint8_t)tty->line[tty->line_len - 1] < 0x20 && tty->line[tty->line_len - 1] != '\t';

    tty->line_len--;
    if ((tty->settings.c_lflag & ECHO) && (tty->settings.c_lflag & ECHOE))
        put_output(tty, "\b \b\b \b", control ? 6 : 3);
}

static void handle_byte(struct tty *tty, char c) {
    uint32_t iflag = tty->settings.c_iflag;
    const uint8_t *cc = tty->settings.c_cc;

    if (c == '\r' && (iflag & IGNCR))
        return;
    if (c == '\r' && (iflag & ICRNL))
        c = '\n';
    else if (c == '\n' && (iflag & INLCR))
        c = '\r';

    // The console's keyboard does this as the key arrives.
    if (tty->is_pty && (uint8_t)c == cc[VINTR] && (tty->settings.c_lflag & ISIG)) {
        tty->line_len = tty->ready;
        process_signal_group(tty->foreground, SIGINT);
        tty_echo_interrupt(tty);
        return;
    }

    if (tty->settings.c_lflag & ICANON) {
        if ((uint8_t)c == cc[VERASE] || c == '\b') {
            erase_last(tty);
            return;
        }
        if ((uint8_t)c == cc[VKILL]) {
            while (tty->line_len > tty->ready)
                erase_last(tty);
            return;
        }
        if ((uint8_t)c == cc[VEOF]) {
            tty->eof_pending = tty->ready == tty->line_len;
            tty->ready = tty->line_len;
            return;
        }
        // A full line keeps room for the newline that ends it.
        if (tty->line_len == LINE_SIZE - 1 && c != '\n')
            return;
    }

    if (tty->line_len == LINE_SIZE)
        return;
    tty->line[tty->line_len++] = c;
    echo(tty, c);
    if (!(tty->settings.c_lflag & ICANON) || c == '\n')
        tty->ready = tty->line_len;
}

// The console's keys are worked through by whoever wants input next, not by the
// interrupt, so a program that turns echo off can't lose the race with what was
// typed just before.
static void take_keys(struct tty *tty) {
    char c;

    while (!tty->is_pty && tty->line_len < LINE_SIZE && keyboard_read(&c, 1) == 1)
        handle_byte(tty, c);
}

static void *input_channel(struct tty *tty) {
    return tty->is_pty ? (void *)tty : keyboard_channel();
}

static size_t bytes_wanted(const struct tty *tty, size_t len) {
    if (tty->settings.c_lflag & ICANON)
        return 1;

    size_t min = tty->settings.c_cc[VMIN];

    return min == 0 ? 1 : min < len ? min : len;
}

int64_t tty_read(struct tty *tty, void *buf, size_t len, bool nonblocking) {
    if (len == 0)
        return 0;

    uint64_t last_change = irq_ticks();
    size_t last_ready = tty->ready;

    for (;;) {
        take_keys(tty);

        if (tty->ready != last_ready) {
            last_ready = tty->ready;
            last_change = irq_ticks();
        }

        const struct termios *s = &tty->settings;
        bool timed = !(s->c_lflag & ICANON) && s->c_cc[VTIME] > 0;
        uint64_t limit = s->c_cc[VTIME] * TICKS_PER_VTIME;
        uint64_t waited = irq_ticks() - last_change;
        bool can_time_out = timed && (s->c_cc[VMIN] == 0 || tty->ready > 0);
        bool timed_out = can_time_out && waited >= limit;
        bool poll_only = !(s->c_lflag & ICANON) && s->c_cc[VMIN] == 0 && !timed;

        if (tty->ready >= bytes_wanted(tty, len) || (tty->ready > 0 && (nonblocking || tty->hung_up)) || poll_only
            || timed_out) {
            size_t count = tty->ready < len ? tty->ready : len;

            memcpy(buf, tty->line, count);
            memmove(tty->line, tty->line + count, tty->line_len - count);
            tty->line_len -= count;
            tty->ready -= count;
            return count;
        }
        if (tty->eof_pending || tty->hung_up) {
            tty->eof_pending = false;
            return 0;
        }
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep_timeout(input_channel(tty), can_time_out ? limit - waited : 0))
            return -EINTR;
    }
}

int64_t tty_write(struct tty *tty, const void *buf, size_t len, bool nonblocking) {
    size_t done = 0;

    while (done < len) {
        if (tty->hung_up)
            return done ? (int64_t)done : -EIO;

        done += put_output(tty, (const char *)buf + done, len - done);
        if (done == len)
            break;
        if (nonblocking)
            return done ? (int64_t)done : -EAGAIN;
        if (!process_sleep(&tty->output))
            return done ? (int64_t)done : -EINTR;
    }
    return done;
}

bool tty_readable(struct tty *tty) {
    take_keys(tty);

    bool polling = !(tty->settings.c_lflag & ICANON) && tty->settings.c_cc[VMIN] == 0;

    return tty->ready > 0 || tty->eof_pending || tty->hung_up || polling;
}

bool tty_writable(const struct tty *tty) {
    return !tty->is_pty || tty->hung_up || tty->output_count < OUTPUT_SIZE;
}

size_t tty_pending(struct tty *tty) {
    take_keys(tty);
    return tty->ready;
}

void tty_get_settings(const struct tty *tty, struct termios *out) {
    *out = tty->settings;
}

void tty_set_settings(struct tty *tty, const struct termios *settings, bool discard_input) {
    bool was_canonical = tty->settings.c_lflag & ICANON;

    if (discard_input) {
        char c;

        while (!tty->is_pty && keyboard_read(&c, 1) == 1)
            ;
        tty->line_len = tty->ready = 0;
        tty->eof_pending = false;
    }

    tty->settings = *settings;

    // A half typed line can be read once nothing is waiting for its end.
    if (was_canonical && !(tty->settings.c_lflag & ICANON))
        tty->ready = tty->line_len;
}

int tty_foreground(const struct tty *tty) {
    return tty->foreground;
}

void tty_set_foreground(struct tty *tty, int pgid) {
    tty->foreground = pgid;
}

void tty_get_size(const struct tty *tty, struct winsize *out) {
    int rows, cols;

    if (tty->is_pty) {
        *out = tty->size;
        return;
    }
    console_size(&rows, &cols);
    *out = (struct winsize){.ws_row = rows, .ws_col = cols};
}

int tty_set_size(struct tty *tty, const struct winsize *size) {
    if (!tty->is_pty)
        return -EINVAL;
    tty->size = *size;
    process_signal_group(tty->foreground, SIGWINCH);
    return 0;
}

void tty_echo_interrupt(struct tty *tty) {
    if (tty->settings.c_lflag & ECHO)
        put_output(tty, "^C", 2);
}

bool tty_signals_enabled(const struct tty *tty) {
    return tty->settings.c_lflag & ISIG;
}

struct tty *tty_open_pty(void) {
    struct tty *tty = kmalloc(sizeof(*tty));
    uint64_t line = tty ? mm_alloc_kernel_page() : 0;
    uint64_t output = line ? mm_alloc_kernel_page() : 0;

    if (!output) {
        if (line)
            mm_free_kernel_page(line);
        kfree(tty);
        return NULL;
    }
    *tty = (struct tty){
        .settings = default_settings, .line = mm_phys_to_virt(line), .is_pty = true, .refs = 1,
        .size = {.ws_row = 24, .ws_col = 80}, .output = mm_phys_to_virt(output),
    };
    return tty;
}

struct tty *tty_ref(struct tty *tty) {
    if (tty && tty->is_pty)
        tty->refs++;
    return tty;
}

void tty_unref(struct tty *tty) {
    if (!tty || !tty->is_pty || --tty->refs > 0)
        return;
    mm_free_kernel_page(mm_virt_to_phys(tty->line));
    mm_free_kernel_page(mm_virt_to_phys(tty->output));
    kfree(tty);
}

// Whoever is reading or writing the slave finds that it's gone.
void tty_close_master(struct tty *tty) {
    tty->hung_up = true;
    process_signal_group(tty->foreground, SIGHUP);
    process_wakeup(tty);
    process_wakeup(&tty->output);
    tty_unref(tty);
}

struct tty *tty_open_slave(struct tty *tty) {
    tty->slaves++;
    return tty_ref(tty);
}

void tty_close_slave(struct tty *tty) {
    if (--tty->slaves == 0)
        process_wakeup(&tty->output);
    tty_unref(tty);
}

int64_t tty_master_read(struct tty *tty, void *buf, size_t len, bool nonblocking) {
    while (tty->output_count == 0) {
        if (tty->slaves == 0)
            return 0;
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(&tty->output))
            return -EINTR;
    }

    size_t count = tty->output_count < len ? tty->output_count : len;

    for (size_t i = 0; i < count; i++)
        ((char *)buf)[i] = tty->output[(tty->output_head + i) % OUTPUT_SIZE];
    tty->output_head = (tty->output_head + count) % OUTPUT_SIZE;
    tty->output_count -= count;
    process_wakeup(&tty->output);
    return count;
}

// What doesn't fit in the line is dropped, as it would be from a keyboard.
int64_t tty_master_write(struct tty *tty, const void *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        handle_byte(tty, ((const char *)buf)[i]);
    process_wakeup(tty);
    return len;
}

int tty_master_poll(const struct tty *tty) {
    return (tty->output_count > 0 ? POLLIN : 0) | POLLOUT | (tty->slaves == 0 ? POLLHUP : 0);
}
