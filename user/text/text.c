#include <aximos.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define CONTROL(c) ((c) & 0x1F)
#define TAB_WIDTH 8
#define HELP "ctrl-s to save, ctrl-q to exit"
#define MESSAGE_SIZE (PATH_MAX + 64)

enum key {
    KEY_NONE = 256,
    KEY_UP,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_EOF,
};

struct line {
    char *text;
    size_t len;
    size_t capacity;
};

// A file is its lines joined by newlines, so "a\n" is "a" and an empty line, and
// saving gives back the bytes that were loaded.
static struct line *lines;
static size_t line_count;
static size_t line_capacity;

static size_t cursor_line;
static size_t cursor_byte;
static int wanted_column; // where up and down aim, so short lines don't lose it
static size_t top_line;
static int left_column;

static char path[PATH_MAX]; // empty until the file has a name
static bool modified;
static char message[MESSAGE_SIZE]; // replaces the help text until the next key
static bool in_prompt;             // the cursor belongs after the message

static int rows = 25;
static int cols = 80;
static char *shown; // what's on screen, so that only changed rows are sent
static char *row_text;
static char *output;
static size_t output_len;

static bool out_of_memory(void) {
    snprintf(message, sizeof(message), "out of memory");
    return false;
}

static bool line_insert_bytes(struct line *l, size_t at, const char *bytes, size_t len) {
    if (l->len + len > l->capacity) {
        size_t capacity = (l->len + len) * 2;
        char *text = realloc(l->text, capacity);

        if (!text)
            return out_of_memory();
        l->text = text;
        l->capacity = capacity;
    }
    memmove(l->text + at + len, l->text + at, l->len - at);
    memcpy(l->text + at, bytes, len);
    l->len += len;
    return true;
}

static bool insert_line(size_t at, const char *text, size_t len) {
    if (line_count == line_capacity) {
        size_t capacity = line_capacity ? line_capacity * 2 : 64;
        struct line *grown = realloc(lines, capacity * sizeof(*lines));

        if (!grown)
            return out_of_memory();
        lines = grown;
        line_capacity = capacity;
    }

    struct line added = {0};

    if (len > 0 && !line_insert_bytes(&added, 0, text, len))
        return false;
    memmove(&lines[at + 1], &lines[at], (line_count - at) * sizeof(*lines));
    lines[at] = added;
    line_count++;
    return true;
}

// Moves the line after this one onto its end.
static bool join_next_line(size_t at) {
    struct line *next = &lines[at + 1];

    if (next->len > 0 && !line_insert_bytes(&lines[at], lines[at].len, next->text, next->len))
        return false;
    free(next->text);
    memmove(next, next + 1, (line_count - at - 2) * sizeof(*lines));
    line_count--;
    return true;
}

static int next_column(int column, char c) {
    return c == '\t' ? (column / TAB_WIDTH + 1) * TAB_WIDTH : column + 1;
}

static int cursor_column(void) {
    int column = 0;

    for (size_t i = 0; i < cursor_byte; i++)
        column = next_column(column, lines[cursor_line].text[i]);
    return column;
}

// Puts the cursor on the last byte of the line that starts at or before wanted_column.
static void aim_cursor(void) {
    struct line *l = &lines[cursor_line];
    int column = 0;

    cursor_byte = 0;
    while (cursor_byte < l->len) {
        int next = next_column(column, l->text[cursor_byte]);

        if (next > wanted_column)
            break;
        column = next;
        cursor_byte++;
    }
}

static void append(const char *bytes, size_t len) {
    memcpy(output + output_len, bytes, len);
    output_len += len;
}

// Sends a row if it changed. Erasing comes first because filling a row moves the
// console's cursor to the next one, and reverse video makes the erase fill the bar.
static void send_row(int row, bool reverse) {
    char *old = shown + (size_t)row * cols;

    if (memcmp(old, row_text, cols) == 0)
        return;
    memcpy(old, row_text, cols);

    int len = cols;

    while (len > 0 && row_text[len - 1] == ' ')
        len--;

    char position[32];

    append(position, snprintf(position, sizeof(position), "\x1b[%d;1H%s\x1b[K", row + 1, reverse ? "\x1b[7m" : ""));
    append(row_text, len);
    if (reverse)
        append("\x1b[0m", 4);
}

static void build_text_row(size_t index) {
    memset(row_text, ' ', cols);
    if (index >= line_count)
        return;

    struct line *l = &lines[index];
    int column = 0;

    for (size_t i = 0; i < l->len && column < left_column + cols; i++) {
        char c = l->text[i];
        int next = next_column(column, c);

        // A stray escape byte in a file mustn't reach the console.
        if (column >= left_column && c != '\t')
            row_text[column - left_column] = c >= 0x20 && c < 0x7F ? c : '?';
        column = next;
    }
}

// The last column stays blank, since writing there would scroll the screen.
static void build_status_row(void) {
    const char *left = message[0] ? message : HELP;
    size_t left_len = strlen(left);
    size_t room = cols - 1;
    char right[PATH_MAX + 64];
    size_t right_len = snprintf(right, sizeof(right), "%s%s  %zu:%d", path[0] ? path : "new file",
                                modified ? " *" : "", cursor_line + 1, cursor_column() + 1);

    memset(row_text, ' ', cols);
    memcpy(row_text, left, left_len < room ? left_len : room);
    if (!in_prompt && left_len + 2 + right_len <= room)
        memcpy(row_text + room - right_len, right, right_len);
}

static void draw(void) {
    int text_rows = rows - 1;
    int column = cursor_column();

    if (cursor_line < top_line)
        top_line = cursor_line;
    if (cursor_line >= top_line + text_rows)
        top_line = cursor_line - text_rows + 1;
    if (column < left_column)
        left_column = column;
    if (column >= left_column + cols)
        left_column = column - cols + 1;

    output_len = 0;
    for (int row = 0; row < text_rows; row++) {
        build_text_row(top_line + row);
        send_row(row, false);
    }
    build_status_row();
    send_row(text_rows, true);

    int cursor_row = cursor_line - top_line;
    int cursor_col = column - left_column;

    if (in_prompt) {
        size_t len = strlen(message);

        cursor_row = text_rows;
        cursor_col = len < (size_t)cols - 1 ? (int)len : cols - 1;
    }

    char position[32];

    append(position, snprintf(position, sizeof(position), "\x1b[%d;%dH", cursor_row + 1, cursor_col + 1));
    write(STDOUT_FILENO, output, output_len);
}

static int read_byte(void) {
    char c;
    ssize_t count;

    while ((count = read(STDIN_FILENO, &c, 1)) < 0 && errno == EINTR)
        ;
    return count == 1 ? (unsigned char)c : KEY_EOF;
}

// The keyboard delivers a whole sequence at once, so if nothing follows an escape
// byte, it was the Escape key.
static bool input_waiting(void) {
    struct pollfd in = {.fd = STDIN_FILENO, .events = POLLIN};

    return poll(&in, 1, 0) > 0;
}

static int read_key(void) {
    int c = read_byte();

    if (c != '\x1b')
        return c;
    if (!input_waiting() || read_byte() != '[')
        return KEY_NONE;

    switch (read_byte()) {
    case 'A':
        return KEY_UP;
    case 'B':
        return KEY_DOWN;
    case 'C':
        return KEY_RIGHT;
    case 'D':
        return KEY_LEFT;
    case 'H':
        return KEY_HOME;
    case 'F':
        return KEY_END;
    case '3':
        read_byte(); // the closing ~
        return KEY_DELETE;
    }
    return KEY_NONE;
}

// Asks for a name on the status bar, returning false if the user cancels.
static bool prompt_for_name(char *name, size_t size) {
    size_t len = 0;
    bool done = false;
    bool accepted = false;

    in_prompt = true;
    while (!done) {
        name[len] = '\0';
        snprintf(message, sizeof(message), "save as (ctrl-q to cancel): %s", name);
        draw();

        int key = read_key();

        if (key == '\n' && len > 0) {
            accepted = done = true;
        } else if (key == CONTROL('q') || key == KEY_EOF) {
            done = true;
        } else if ((key == 0x7F || key == '\b') && len > 0) {
            len--;
        } else if (key >= 0x20 && key < 0x7F && len + 1 < size) {
            name[len++] = key;
        }
    }
    in_prompt = false;
    message[0] = '\0';
    return accepted;
}

static bool write_all(int fd, const char *bytes, size_t len) {
    while (len > 0) {
        ssize_t count = write(fd, bytes, len);

        if (count <= 0)
            return false;
        bytes += count;
        len -= count;
    }
    return true;
}

// Writes a copy and renames it over the file, so a full disk can't cost the old one.
static bool write_file(const char *target, const char *bytes, size_t len) {
    char temporary[PATH_MAX + 32];

    if (snprintf(temporary, sizeof(temporary), "%s.%d.tmp", target, getpid()) >= (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return false;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);

    if (fd < 0)
        return false;

    bool ok = write_all(fd, bytes, len);
    int saved_errno = errno;

    close(fd);
    if (ok && rename(temporary, target) == 0)
        return true;
    if (ok)
        saved_errno = errno;
    unlink(temporary);
    errno = saved_errno;
    return false;
}

static void save(void) {
    if (!path[0]) {
        char name[PATH_MAX];
        struct stat st;

        if (!prompt_for_name(name, sizeof(name))) {
            snprintf(message, sizeof(message), "not saved");
            return;
        }
        if (stat(name, &st) == 0) {
            snprintf(message, sizeof(message), "not saved: %s already exists", name);
            return;
        }
        strcpy(path, name);
    }

    size_t size = line_count - 1;

    for (size_t i = 0; i < line_count; i++)
        size += lines[i].len;

    char *bytes = malloc(size + 1);

    if (!bytes) {
        out_of_memory();
        return;
    }

    size_t at = 0;

    for (size_t i = 0; i < line_count; i++) {
        if (lines[i].len > 0)
            memcpy(bytes + at, lines[i].text, lines[i].len);
        at += lines[i].len;
        if (i + 1 < line_count)
            bytes[at++] = '\n';
    }

    if (write_file(path, bytes, size)) {
        modified = false;
        snprintf(message, sizeof(message), "saved %zu bytes", size);
    } else {
        snprintf(message, sizeof(message), "not saved: %s", strerror(errno));
    }
    free(bytes);
}

static void insert_text(char c) {
    if (line_insert_bytes(&lines[cursor_line], cursor_byte, &c, 1)) {
        cursor_byte++;
        modified = true;
    }
}

static void insert_newline(void) {
    struct line *l = &lines[cursor_line];

    if (!insert_line(cursor_line + 1, l->text ? l->text + cursor_byte : "", l->len - cursor_byte))
        return;
    lines[cursor_line].len = cursor_byte;
    cursor_line++;
    cursor_byte = 0;
    modified = true;
}

static void delete_forward(void) {
    struct line *l = &lines[cursor_line];

    if (cursor_byte < l->len) {
        memmove(l->text + cursor_byte, l->text + cursor_byte + 1, l->len - cursor_byte - 1);
        l->len--;
        modified = true;
    } else if (cursor_line + 1 < line_count && join_next_line(cursor_line)) {
        modified = true;
    }
}

static void delete_backward(void) {
    if (cursor_byte > 0) {
        cursor_byte--;
    } else if (cursor_line > 0) {
        cursor_line--;
        cursor_byte = lines[cursor_line].len;
    } else {
        return;
    }
    delete_forward();
}

// Returns false once the user has chosen to exit.
static bool handle_key(int key) {
    static bool warned;
    bool was_warned = warned;
    bool vertical = false;

    warned = false;
    message[0] = '\0';

    switch (key) {
    case KEY_EOF:
        return false;
    case CONTROL('q'):
        if (!modified || was_warned)
            return false;
        warned = true;
        snprintf(message, sizeof(message), "unsaved changes! ctrl-q again to exit without saving");
        break;
    case CONTROL('s'):
        save();
        break;
    case KEY_UP:
        vertical = true;
        if (cursor_line > 0) {
            cursor_line--;
            aim_cursor();
        }
        break;
    case KEY_DOWN:
        vertical = true;
        if (cursor_line + 1 < line_count) {
            cursor_line++;
            aim_cursor();
        }
        break;
    case KEY_LEFT:
        if (cursor_byte > 0) {
            cursor_byte--;
        } else if (cursor_line > 0) {
            cursor_line--;
            cursor_byte = lines[cursor_line].len;
        }
        break;
    case KEY_RIGHT:
        if (cursor_byte < lines[cursor_line].len) {
            cursor_byte++;
        } else if (cursor_line + 1 < line_count) {
            cursor_line++;
            cursor_byte = 0;
        }
        break;
    case KEY_HOME:
        cursor_byte = 0;
        break;
    case KEY_END:
        cursor_byte = lines[cursor_line].len;
        break;
    case KEY_DELETE:
        delete_forward();
        break;
    case 0x7F:
    case '\b':
        delete_backward();
        break;
    case '\n':
        insert_newline();
        break;
    default:
        if (key == '\t' || (key >= 0x20 && key < 0x7F))
            insert_text(key);
    }

    if (!vertical)
        wanted_column = cursor_column();
    return true;
}

static void fail(const char *what) {
    dprintf(STDERR_FILENO, "text: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void load(const char *name) {
    struct stat st;

    if (strlen(name) >= sizeof(path)) {
        errno = ENAMETOOLONG;
        fail(name);
    }
    strcpy(path, name);

    if (stat(name, &st) < 0) {
        if (errno != ENOENT)
            fail(name);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        dprintf(STDERR_FILENO, "text: %s: not a regular file\n", name);
        exit(1);
    }

    int fd = open(name, O_RDONLY);
    char *bytes = malloc(st.st_size + 1);
    size_t size = 0;
    ssize_t count;

    if (fd < 0 || !bytes)
        fail(name);
    while (size < (size_t)st.st_size && (count = read(fd, bytes + size, st.st_size - size)) > 0)
        size += count;
    close(fd);

    line_count = 0;
    for (size_t start = 0;;) {
        const char *newline = memchr(bytes + start, '\n', size - start);
        size_t end = newline ? (size_t)(newline - bytes) : size;

        if (!insert_line(line_count, bytes + start, end - start))
            fail(name);
        if (!newline)
            break;
        start = end + 1;
    }
    free(bytes);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: text [file]\n"
                            "Edits a text file.\n"
                            "  Ctrl+S  save\n"
                            "  Ctrl+Q  quit\n");

    if (argc > 2) {
        dprintf(STDERR_FILENO, "usage: text [file]\n");
        return 1;
    }

    if (!insert_line(0, "", 0))
        return 1;
    if (argc == 2)
        load(argv[1]);

    if (termsize(&rows, &cols) < 0 || rows < 2 || cols < 2) {
        dprintf(STDERR_FILENO, "text: the screen is too small\n");
        return 1;
    }

    shown = calloc((size_t)rows, cols);
    row_text = malloc(cols);
    output = malloc((size_t)rows * (cols + 32));
    if (!shown || !row_text || !output)
        return 1;

    // Ctrl+C mustn't throw away unsaved work.
    signal(SIGINT, SIG_IGN);

    // Keys come straight through, and it does the drawing.
    struct termios settings, raw;
    bool on_terminal = tcgetattr(STDIN_FILENO, &settings) == 0;

    if (on_terminal) {
        raw = settings;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    printf("\x1b[H\x1b[2J");
    do {
        draw();
    } while (handle_key(read_key()));
    printf("\x1b[H\x1b[2J");

    if (on_terminal)
        tcsetattr(STDIN_FILENO, TCSANOW, &settings);
    return 0;
}
