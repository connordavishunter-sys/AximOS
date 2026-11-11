#include <errno.h>
#include <font8x16.h>
#include <gui.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define COLS 80 // to start with
#define ROWS 25
#define MAX_COLS 320
#define MAX_ROWS 200
#define CELL_WIDTH 8
#define CELL_HEIGHT 16
#define PADDING 4
#define TAB_WIDTH 8
#define MAX_PARAMS 8
#define PARAM_HIDE_CURSOR 25
#define SCROLLBACK 1000 // lines kept after scrolling off the top
#define WHEEL_LINES 3
#define DRAG_SCROLL_MS 50 // between scrolls while a selection is dragged out of the window
#define KEY_C 0x2E
#define KEY_V 0x2F

// The console's colours, then the defaults, which aren't any of them.
#define BRIGHT 8
#define DEFAULT_FOREGROUND 16
#define DEFAULT_BACKGROUND 17
static const uint32_t palette[18] = {
    0x000000, 0xCC3333, 0x33AA33, 0xCCAA33, 0x3366CC, 0xAA44AA, 0x33AAAA, 0xC0C0C0,
    0x666666, 0xFF6666, 0x66DD66, 0xFFDD66, 0x6699FF, 0xDD77DD, 0x66DDDD, 0xFFFFFF,
    0xD8DEE9, 0x161A22,
};

struct cell {
    char c;
    uint8_t foreground, background;
};

static struct gui_window *window;
static struct cell cells[MAX_ROWS][MAX_COLS];
static bool dirty[MAX_ROWS]; // rows of the screen to draw again

// Lines that scrolled off the top, oldest first from history_start, and how many
// of them the view is scrolled back by.
static struct cell history[SCROLLBACK][MAX_COLS];
static int history_start, history_count, view;

// What the mouse has selected, from where it was pressed to where it's been
// dragged, by line counting from the oldest kept, and column.
struct place {
    int line, col;
};
static struct place select_from, select_to;
static bool selecting, selected;
static int drag_x, drag_y; // where the mouse is while selecting
static int cols = COLS, rows = ROWS;
static int row, col;
static bool cursor_hidden, focused;

// What the next character is drawn in.
static uint8_t foreground = DEFAULT_FOREGROUND, background = DEFAULT_BACKGROUND;
static bool bold, reverse;

static enum { PARSE_TEXT, PARSE_ESCAPE, PARSE_CSI } state;
static int params[MAX_PARAMS], param_count;
static bool private_sequence;

static void set_cell(int r, int c, char ch) {
    cells[r][c] = (struct cell){ch, reverse ? background : foreground, reverse ? foreground : background};
    dirty[r] = true;
}

static void clear_cells(int r, int from, int to) {
    for (int c = from; c < to; c++)
        cells[r][c] = (struct cell){' ', foreground, background};
    dirty[r] = true;
}

static struct cell *history_line(int i) {
    return history[(history_start + i) % SCROLLBACK];
}

static void mark_all_dirty(void) {
    for (int r = 0; r < rows; r++)
        dirty[r] = true;
}

// Keeps a line leaving the top of the screen, losing the oldest when full. A view
// scrolled back stays on the lines it shows.
static void keep_line(const struct cell *line) {
    struct cell *to = history_line(history_count);

    if (history_count < SCROLLBACK) {
        history_count++;
    } else {
        // Every kept line moves up one, and the selection with them.
        history_start = (history_start + 1) % SCROLLBACK;
        select_from.line--;
        select_to.line--;
        selected &= select_from.line >= 0 && select_to.line >= 0;
    }
    memcpy(to, line, cols * sizeof(*line));
    for (int c = cols; c < MAX_COLS; c++)
        to[c] = (struct cell){' ', DEFAULT_FOREGROUND, DEFAULT_BACKGROUND};
    if (view) {
        view = view < history_count ? view + 1 : history_count;
        mark_all_dirty();
    }
}

static void new_line(void) {
    col = 0;
    if (++row < rows)
        return;
    row = rows - 1;
    keep_line(cells[0]);
    memmove(cells[0], cells[1], (rows - 1) * sizeof(cells[0]));
    clear_cells(rows - 1, 0, cols);
    mark_all_dirty();
}

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static int param_or_one(int index) {
    return index < param_count && params[index] > 0 ? params[index] : 1;
}

// Bold shows as the bright version, as the font has no heavier one.
static void set_graphics(void) {
    for (int i = 0; i < (param_count ? param_count : 1); i++) {
        int p = params[i];

        if (p == 0) {
            foreground = DEFAULT_FOREGROUND;
            background = DEFAULT_BACKGROUND;
            bold = reverse = false;
        } else if (p == 1 || p == 22) {
            bold = p == 1;
        } else if (p == 7 || p == 27) {
            reverse = p == 7;
        } else if (p >= 30 && p <= 37) {
            foreground = p - 30 + (bold ? BRIGHT : 0);
        } else if (p == 39) {
            foreground = DEFAULT_FOREGROUND;
        } else if (p >= 40 && p <= 47) {
            background = p - 40;
        } else if (p == 49) {
            background = DEFAULT_BACKGROUND;
        } else if (p >= 90 && p <= 97) {
            foreground = p - 90 + BRIGHT;
        } else if (p >= 100 && p <= 107) {
            background = p - 100 + BRIGHT;
        } else if (p == 38 || p == 48) {
            return; // what follows says which of the many colours there's no room for
        }
    }
}

static void run_csi(char final) {
    int mode = param_count > 0 ? params[0] : 0;

    dirty[row] = true; // where the cursor was
    if (private_sequence) {
        if (mode == PARAM_HIDE_CURSOR && (final == 'h' || final == 'l'))
            cursor_hidden = final == 'l';
        return;
    }

    switch (final) {
    case 'A':
        row = clamp(row - param_or_one(0), 0, rows - 1);
        break;
    case 'B':
        row = clamp(row + param_or_one(0), 0, rows - 1);
        break;
    case 'C':
        col = clamp(col + param_or_one(0), 0, cols - 1);
        break;
    case 'D':
        col = clamp(col - param_or_one(0), 0, cols - 1);
        break;
    case 'G':
        col = clamp(param_or_one(0) - 1, 0, cols - 1);
        break;
    case 'H':
    case 'f':
        row = clamp(param_or_one(0) - 1, 0, rows - 1);
        col = clamp(param_or_one(1) - 1, 0, cols - 1);
        break;
    case 'J':
        if (mode == 2) {
            for (int r = 0; r < rows; r++)
                clear_cells(r, 0, cols);
            break;
        }
        clear_cells(row, col, cols);
        for (int r = row + 1; r < rows; r++)
            clear_cells(r, 0, cols);
        break;
    case 'K':
        clear_cells(row, mode == 2 ? 0 : mode == 1 ? 0 : col, mode == 1 ? col + 1 : cols);
        break;
    case 'm':
        set_graphics();
        break;
    }
}

static void put_text(char c) {
    dirty[row] = true;
    switch (c) {
    case '\n':
        new_line();
        return;
    case '\r':
        col = 0;
        return;
    case '\b':
        if (col > 0)
            col--;
        return;
    case '\t':
        do {
            set_cell(row, col, ' ');
            if (++col == cols)
                new_line();
        } while (col % TAB_WIDTH != 0);
        return;
    case '\a':
        return;
    case '\x1b':
        state = PARSE_ESCAPE;
        return;
    }

    // Other control characters and non-ASCII bytes have no glyph here.
    set_cell(row, col, (uint8_t)c >= 0x20 && (uint8_t)c < 0x7F ? c : '?');
    if (++col == cols)
        new_line();
}

static void put_byte(char c) {
    switch (state) {
    case PARSE_TEXT:
        put_text(c);
        break;
    case PARSE_ESCAPE:
        state = c == '[' ? PARSE_CSI : PARSE_TEXT;
        param_count = 0;
        private_sequence = false;
        memset(params, 0, sizeof(params));
        break;
    case PARSE_CSI:
        if (c == '?') {
            private_sequence = true;
        } else if (c >= '0' && c <= '9') {
            if (param_count == 0)
                param_count = 1;
            params[param_count - 1] = params[param_count - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (param_count == 0)
                param_count = 1;
            if (param_count < MAX_PARAMS)
                param_count++;
        } else {
            run_csi(c);
            state = PARSE_TEXT;
        }
        break;
    }
}

// A line counting from the oldest kept, through the scrollback and onto the screen.
static const struct cell *line_cells(int line) {
    return line < history_count ? history_line(line) : cells[line - history_count];
}

// What's shown on row r of the window, which is in the scrollback when scrolled back.
static int shown_line(int r) {
    return history_count - view + r;
}

static long order(struct place p) {
    return (long)p.line * MAX_COLS + p.col;
}

static bool is_selected(int line, int c) {
    long from = order(select_from), to = order(select_to), at = order((struct place){line, c});

    return selected && at >= (from < to ? from : to) && at <= (from < to ? to : from);
}

static void draw_cell(int r, int c) {
    struct cell cell = line_cells(shown_line(r))[c];
    bool cursor = r - view == row && c == col && !cursor_hidden;
    uint32_t fg = palette[cell.foreground], bg = palette[cell.background];
    int x = PADDING + c * CELL_WIDTH, y = PADDING + r * CELL_HEIGHT;

    // A solid block when focused, an outline when not, and what's selected reversed.
    if ((cursor && focused) != is_selected(shown_line(r), c)) {
        uint32_t swap = fg;
        fg = bg;
        bg = swap;
    }
    gui_fill(&window->canvas, x, y, CELL_WIDTH, CELL_HEIGHT, bg);
    for (int line = 0; line < CELL_HEIGHT; line++) {
        for (int bit = 0; bit < CELL_WIDTH; bit++) {
            if (font8x16[(uint8_t)cell.c & 0x7F][line] & 0x80 >> bit)
                gui_fill(&window->canvas, x + bit, y + line, 1, 1, fg);
        }
    }
    if (cursor && !focused) {
        gui_fill(&window->canvas, x, y, CELL_WIDTH, 1, fg);
        gui_fill(&window->canvas, x, y + CELL_HEIGHT - 1, CELL_WIDTH, 1, fg);
        gui_fill(&window->canvas, x, y, 1, CELL_HEIGHT, fg);
        gui_fill(&window->canvas, x + CELL_WIDTH - 1, y, 1, CELL_HEIGHT, fg);
    }
}

// A thumb in the right margin while scrolled back, showing where.
static void draw_scrollbar(void) {
    int x = window->canvas.width - PADDING, height = window->canvas.height, total = history_count + rows;

    gui_fill(&window->canvas, x, 0, PADDING, height, palette[DEFAULT_BACKGROUND]);
    if (view)
        gui_fill(&window->canvas, x + 1, (history_count - view) * height / total, PADDING - 2, rows * height / total,
                 palette[BRIGHT]);
    gui_update(window, x, 0, PADDING, height);
}

static void draw(void) {
    static int drawn_view;
    int first = rows, last = -1;

    // Changes on the screen move about the window while scrolled back.
    for (int r = 0; view && r < rows; r++) {
        if (dirty[r])
            mark_all_dirty();
    }
    if (view != drawn_view || (view && dirty[0]))
        draw_scrollbar();
    drawn_view = view;
    for (int r = 0; r < rows; r++) {
        if (!dirty[r])
            continue;
        for (int c = 0; c < cols; c++)
            draw_cell(r, c);
        dirty[r] = false;
        first = r < first ? r : first;
        last = r;
    }
    if (last >= 0)
        gui_update(window, PADDING, PADDING + first * CELL_HEIGHT, cols * CELL_WIDTH, (last - first + 1) * CELL_HEIGHT);
}

static void write_all(int fd, const char *s, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, s, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        s += n;
        len -= n;
    }
}

// What a key sends, as the console's keyboard would. Alt adds an escape first.
// Returns whether it sent anything.
static bool send_key(int master, const struct key_event *key) {
    const char *sequence = NULL;
    char text = key->text;

    switch (key->code) {
    case KEY_UP: sequence = "\x1b[A"; break;
    case KEY_DOWN: sequence = "\x1b[B"; break;
    case KEY_RIGHT: sequence = "\x1b[C"; break;
    case KEY_LEFT: sequence = "\x1b[D"; break;
    case KEY_HOME: sequence = "\x1b[H"; break;
    case KEY_END: sequence = "\x1b[F"; break;
    case KEY_INSERT: sequence = "\x1b[2~"; break;
    case KEY_DELETE: sequence = "\x1b[3~"; break;
    case KEY_PAGE_UP: sequence = "\x1b[5~"; break;
    case KEY_PAGE_DOWN: sequence = "\x1b[6~"; break;
    }
    if (sequence) {
        write_all(master, sequence, strlen(sequence));
    } else if (text) {
        if (key->modifiers & KEY_MOD_ALT)
            write_all(master, "\x1b", 1);
        write_all(master, &text, 1);
    }
    return sequence || text;
}

static void scroll_view(int lines) {
    view = clamp(view + lines, 0, history_count);
    mark_all_dirty();
}

// The cell under a point in the window.
static struct place place_at(int x, int y) {
    return (struct place){shown_line(clamp((y - PADDING) / CELL_HEIGHT, 0, rows - 1)),
                          clamp((x - PADDING) / CELL_WIDTH, 0, cols - 1)};
}

// The mouse selects by dragging. A click on its own selects nothing.
static void mouse(const struct gui_message *e) {
    if (e->type == GUI_MOUSE_BUTTON && (e->changed & 1)) {
        selecting = e->buttons & 1;
        if (selecting) {
            select_from = select_to = place_at(e->x, e->y);
            selected = false;
        }
    } else if (e->type == GUI_MOUSE_MOVE && selecting) {
        select_to = place_at(e->x, e->y);
        selected = order(select_from) != order(select_to);
    }
    drag_x = e->x;
    drag_y = e->y;
    mark_all_dirty();
}

// How many lines back to scroll while a selection is dragged above the window,
// or forward when below it, more the further out it is.
static int drag_lines(void) {
    int bottom = PADDING + rows * CELL_HEIGHT;

    if (!selecting)
        return 0;
    if (drag_y < PADDING)
        return 1 + (PADDING - drag_y) / CELL_HEIGHT;
    if (drag_y >= bottom)
        return -(1 + (drag_y - bottom) / CELL_HEIGHT);
    return 0;
}

static long now_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Scrolls every DRAG_SCROLL_MS while the selection is dragged out of the window,
// taking the selection along, and returns how long until the next scroll, or -1.
static int drag_scroll(void) {
    static long last;
    long now = now_ms();
    int lines = drag_lines();

    if (!lines)
        return -1;
    if (now - last >= DRAG_SCROLL_MS) {
        last = now;
        scroll_view(lines);
        select_to = place_at(drag_x, drag_y);
        selected = order(select_from) != order(select_to);
    }
    return (int)(DRAG_SCROLL_MS - (now - last));
}

// Each line's text, without the spaces at its end, with a line break between them.
static void copy_selection(void) {
    struct place from = order(select_from) < order(select_to) ? select_from : select_to;
    struct place to = order(select_from) < order(select_to) ? select_to : select_from;
    char *text = selected ? malloc((size_t)(to.line - from.line + 1) * (MAX_COLS + 1)) : NULL;
    size_t length = 0;

    if (!text)
        return;
    for (int line = from.line; line <= to.line; line++) {
        const struct cell *cells_of = line_cells(line);
        int first = line == from.line ? from.col : 0, last = line == to.line ? to.col : cols - 1;

        while (last >= first && cells_of[last].c == ' ')
            last--;
        for (int c = first; c <= last; c++)
            text[length++] = cells_of[c].c;
        if (line < to.line)
            text[length++] = '\n';
    }
    gui_clipboard_set(text, length);
    free(text);
}

// Pasting types what's on the clipboard, with Enter for each line break.
static void paste(int master) {
    size_t length;
    char *text = gui_clipboard_get(&length);

    for (size_t i = 0; i < length; i++)
        text[i] = text[i] == '\n' ? '\r' : text[i];
    write_all(master, text, length);
    free(text);
}

static void paint_background(void) {
    gui_fill(&window->canvas, 0, 0, window->canvas.width, window->canvas.height, palette[DEFAULT_BACKGROUND]);
    mark_all_dirty();
}

// Fits as many cells as the new size holds, keeping the cursor's line on the
// screen, and tells the programs on the terminal.
static void resize(int master, int width, int height) {
    int new_cols = clamp((width - 2 * PADDING) / CELL_WIDTH, 1, MAX_COLS);
    int new_rows = clamp((height - 2 * PADDING) / CELL_HEIGHT, 1, MAX_ROWS);
    int lost = row - (new_rows - 1);

    if (!gui_resize(window, width, height))
        return;
    if (lost > 0) {
        for (int r = 0; r < lost; r++)
            keep_line(cells[r]);
        memmove(cells[0], cells[lost], (rows - lost) * sizeof(cells[0]));
        row -= lost;
    }

    // What comes into view is blank.
    int kept = lost > 0 ? rows - lost : rows;

    for (int r = 0; r < new_rows; r++) {
        for (int c = r < kept ? cols : 0; c < new_cols; c++)
            cells[r][c] = (struct cell){' ', DEFAULT_FOREGROUND, DEFAULT_BACKGROUND};
    }
    cols = new_cols;
    rows = new_rows;
    col = clamp(col, 0, cols - 1);
    view = clamp(view, 0, history_count);

    struct winsize size = {.ws_row = rows, .ws_col = cols, .ws_xpixel = cols * CELL_WIDTH, .ws_ypixel = rows * CELL_HEIGHT};

    ioctl(master, TIOCSWINSZ, &size);
    paint_background();
    draw();
    gui_show(window);
}

static pid_t start_shell(int *master, char **command) {
    struct winsize size = {.ws_row = rows, .ws_col = cols, .ws_xpixel = cols * CELL_WIDTH, .ws_ypixel = rows * CELL_HEIGHT};
    int slave;

    if (openpty(master, &slave, NULL, NULL, &size) < 0)
        return -1;

    pid_t child = fork();

    if (child == 0) {
        close(*master);
        close(gui_fd());
        login_tty(slave);
        if (*command)
            execvp(command[0], command);
        else
            execl("/system/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    close(slave);
    return child;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: term [command [argument]...]\n"
                            "Opens a terminal window running the shell, or the command.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "term: wm isn't running\n");
        return 1;
    }

    int master;

    window = gui_create_window("Terminal", cols * CELL_WIDTH + 2 * PADDING, rows * CELL_HEIGHT + 2 * PADDING);
    pid_t child = window ? start_shell(&master, argv + 1) : -1;

    if (child < 0) {
        dprintf(STDERR_FILENO, "term: %s\n", window ? strerror(errno) : "couldn't make a window");
        return 1;
    }
    for (int r = 0; r < rows; r++)
        clear_cells(r, 0, cols);
    gui_set_cursor(window, GUI_CURSOR_TEXT);
    paint_background();
    draw();
    gui_show(window);

    bool running = true;

    while (running) {
        struct pollfd fds[2] = {{.fd = gui_fd(), .events = POLLIN}, {.fd = master, .events = POLLIN}};
        char buf[4096];

        int timeout = drag_scroll();

        draw(); // what came last time round, and any scrolling
        if (poll(fds, 2, timeout) < 0 && errno != EINTR)
            break;

        // The shell's output, ending when everything on the terminal has exited.
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(master, buf, sizeof(buf));

            running = n > 0 || (n < 0 && (errno == EINTR || errno == EAGAIN));
            for (ssize_t i = 0; i < n; i++)
                put_byte(buf[i]);
            dirty[row] = true; // where the cursor is now
        }

        struct gui_message e;

        if ((fds[0].revents & (POLLIN | POLLHUP)) && gui_next_event(&e)) {
            bool shifted = e.key.modifiers & KEY_MOD_SHIFT;
            bool ctrl_shift = shifted && (e.key.modifiers & KEY_MOD_CTRL);

            // Shift+Page Up and Page Down scroll back and forth, and other keys go
            // to the bottom as they go to the terminal.
            // Ctrl+Shift+C and V copy and paste, leaving Ctrl+C to interrupt.
            if (e.type == GUI_KEY && e.key.pressed && ctrl_shift && e.key.code == KEY_C) {
                copy_selection();
            } else if (e.type == GUI_KEY && e.key.pressed && ctrl_shift && e.key.code == KEY_V) {
                if (view)
                    scroll_view(-view);
                paste(master);
            } else if (e.type == GUI_KEY && e.key.pressed && shifted
                && (e.key.code == KEY_PAGE_UP || e.key.code == KEY_PAGE_DOWN)) {
                scroll_view((e.key.code == KEY_PAGE_UP ? 1 : -1) * (rows > 1 ? rows - 1 : 1));
            } else if (e.type == GUI_KEY && e.key.pressed) {
                if (send_key(master, &e.key) && view)
                    scroll_view(-view);
            } else if (e.type == GUI_MOUSE_MOVE || e.type == GUI_MOUSE_BUTTON) {
                mouse(&e);
            } else if (e.type == GUI_MOUSE_WHEEL) {
                scroll_view(-e.wheel * WHEEL_LINES);
            } else if (e.type == GUI_FOCUS) {
                focused = e.focused;
                dirty[row] = true;
            } else if (e.type == GUI_RESIZE) {
                resize(master, e.width, e.height);
            } else if (e.type == GUI_CLOSE) {
                running = false;
            }
        } else if (fds[0].revents & (POLLIN | POLLHUP)) {
            running = false; // wm has gone
        }
    }

    // Closing the master hangs up whatever is still running on the terminal.
    close(master);
    gui_destroy_window(window);
    return 0;
}
