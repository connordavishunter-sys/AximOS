#include <ctype.h>
#include <errno.h>
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

// The console's colours, then the defaults, which aren't any of them and follow
// the theme.
#define BRIGHT 8
#define DEFAULT_FOREGROUND 16
#define DEFAULT_BACKGROUND 17
// How see-through the background is, in its top byte, which wm reads: as much as title bars.
#define SEE_THROUGH 0x38000000
static uint32_t palette[18] = {
    0x000000, 0xCC3333, 0x33AA33, 0xCCAA33, 0x3366CC, 0xAA44AA, 0x33AAAA, 0xC0C0C0,
    0x666666, 0xFF6666, 0x66DD66, 0xFFDD66, 0x6699FF, 0xDD77DD, 0x66DDDD, 0xFFFFFF,
    0xD8DEE9, 0x161A22,
};

struct cell {
    char c;
    uint8_t foreground, background;
};

#define MAX_TABS 8
#define TAB_BAR 26 // across the top, when there's more than one tab
#define TAB_WIDTH_MAX 160
#define MENU_WIDTH 160
#define MENU_ROW 22
#define KEY_T 0x14
#define KEY_W 0x11
#define KEY_F 0x21

// What the mouse has selected, from where it was pressed to where it's been
// dragged, by line counting from the oldest kept, and column.
struct place {
    int line, col;
};

// Each tab is a shell on a terminal of its own, with what's on its screen, the
// lines that scrolled off the top, oldest first from history_start, and how many
// of them the view is scrolled back by.
struct session {
    int master;
    pid_t child;
    int number; // in the order they were opened, for its name
    struct cell cells[MAX_ROWS][MAX_COLS];
    struct cell history[SCROLLBACK][MAX_COLS];
    int history_start, history_count, view;
    struct place select_from, select_to;
    bool selected;
    int row, col;
    bool cursor_hidden;

    // What the next character is drawn in, and the escape sequence being read.
    uint8_t foreground, background;
    bool bold, reverse;
    enum { PARSE_TEXT, PARSE_ESCAPE, PARSE_CSI } state;
    int params[MAX_PARAMS], param_count;
    bool private_sequence;
};

static struct gui_window *window;
static struct session *sessions[MAX_TABS];
static int session_count, active;
static struct session *t; // the one being written to, or shown
static bool dirty[MAX_ROWS]; // rows of the screen to draw again
static int cols = COLS, rows = ROWS, top = PADDING;
static bool focused, selecting;
static int drag_x, drag_y; // where the mouse is while selecting
static long last_press;    // for double clicks

// Over the cells: a menu from a right-click, searching what's kept, and the help.
static const char *const menu_items[] = {"Copy", "Paste", "New tab", "Find...", "Keyboard shortcuts"};
#define MENU_ITEMS ((int)(sizeof(menu_items) / sizeof(*menu_items)))
static bool menu_open, help_shown, searching;
static int menu_x, menu_y;
static char query[64], search_status[64];
static int found_line = -1;

static const char *const help[] = {
    "Ctrl+Shift+C, V      Copy and paste",
    "Ctrl+Shift+T, W      New tab, close the tab",
    "Ctrl+Tab             Next tab",
    "Ctrl+Shift+F         Find in what's kept",
    "Enter, Shift+Enter   Find older, newer",
    "Shift+Page Up, Down  Scroll back and forth",
    "Double click         Select a word",
    "Escape               Close this",
};
#define HELP_LINES ((int)(sizeof(help) / sizeof(*help)))

static void set_cell(int r, int c, char ch) {
    t->cells[r][c] = (struct cell){ch, t->reverse ? t->background : t->foreground, t->reverse ? t->foreground : t->background};
    dirty[r] = true;
}

static void clear_cells(int r, int from, int to) {
    for (int c = from; c < to; c++)
        t->cells[r][c] = (struct cell){' ', t->foreground, t->background};
    dirty[r] = true;
}

static struct cell *history_line(int i) {
    return t->history[(t->history_start + i) % SCROLLBACK];
}

static void mark_all_dirty(void) {
    for (int r = 0; r < rows; r++)
        dirty[r] = true;
}

// Keeps a line leaving the top of the screen, losing the oldest when full. A view
// scrolled back stays on the lines it shows.
static void keep_line(const struct cell *line) {
    struct cell *to = history_line(t->history_count);

    if (t->history_count < SCROLLBACK) {
        t->history_count++;
    } else {
        // Every kept line moves up one, and the selection with them.
        t->history_start = (t->history_start + 1) % SCROLLBACK;
        t->select_from.line--;
        t->select_to.line--;
        t->selected &= t->select_from.line >= 0 && t->select_to.line >= 0;
    }
    memcpy(to, line, cols * sizeof(*line));
    for (int c = cols; c < MAX_COLS; c++)
        to[c] = (struct cell){' ', DEFAULT_FOREGROUND, DEFAULT_BACKGROUND};
    if (t->view) {
        t->view = t->view < t->history_count ? t->view + 1 : t->history_count;
        mark_all_dirty();
    }
}

static void new_line(void) {
    t->col = 0;
    if (++t->row < rows)
        return;
    t->row = rows - 1;
    keep_line(t->cells[0]);
    memmove(t->cells[0], t->cells[1], (rows - 1) * sizeof(t->cells[0]));
    clear_cells(rows - 1, 0, cols);
    mark_all_dirty();
}

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static int param_or_one(int index) {
    return index < t->param_count && t->params[index] > 0 ? t->params[index] : 1;
}

// Bold shows as the bright version, as the font has no heavier one.
static void set_graphics(void) {
    for (int i = 0; i < (t->param_count ? t->param_count : 1); i++) {
        int p = t->params[i];

        if (p == 0) {
            t->foreground = DEFAULT_FOREGROUND;
            t->background = DEFAULT_BACKGROUND;
            t->bold = t->reverse = false;
        } else if (p == 1 || p == 22) {
            t->bold = p == 1;
        } else if (p == 7 || p == 27) {
            t->reverse = p == 7;
        } else if (p >= 30 && p <= 37) {
            t->foreground = p - 30 + (t->bold ? BRIGHT : 0);
        } else if (p == 39) {
            t->foreground = DEFAULT_FOREGROUND;
        } else if (p >= 40 && p <= 47) {
            t->background = p - 40;
        } else if (p == 49) {
            t->background = DEFAULT_BACKGROUND;
        } else if (p >= 90 && p <= 97) {
            t->foreground = p - 90 + BRIGHT;
        } else if (p >= 100 && p <= 107) {
            t->background = p - 100 + BRIGHT;
        } else if (p == 38 || p == 48) {
            return; // what follows says which of the many colours there's no room for
        }
    }
}

static void run_csi(char final) {
    int mode = t->param_count > 0 ? t->params[0] : 0;

    dirty[t->row] = true; // where the cursor was
    if (t->private_sequence) {
        if (mode == PARAM_HIDE_CURSOR && (final == 'h' || final == 'l'))
            t->cursor_hidden = final == 'l';
        return;
    }

    switch (final) {
    case 'A':
        t->row = clamp(t->row - param_or_one(0), 0, rows - 1);
        break;
    case 'B':
        t->row = clamp(t->row + param_or_one(0), 0, rows - 1);
        break;
    case 'C':
        t->col = clamp(t->col + param_or_one(0), 0, cols - 1);
        break;
    case 'D':
        t->col = clamp(t->col - param_or_one(0), 0, cols - 1);
        break;
    case 'G':
        t->col = clamp(param_or_one(0) - 1, 0, cols - 1);
        break;
    case 'H':
    case 'f':
        t->row = clamp(param_or_one(0) - 1, 0, rows - 1);
        t->col = clamp(param_or_one(1) - 1, 0, cols - 1);
        break;
    case 'J':
        if (mode == 2) {
            for (int r = 0; r < rows; r++)
                clear_cells(r, 0, cols);
            break;
        }
        clear_cells(t->row, t->col, cols);
        for (int r = t->row + 1; r < rows; r++)
            clear_cells(r, 0, cols);
        break;
    case 'K':
        clear_cells(t->row, mode == 2 ? 0 : mode == 1 ? 0 : t->col, mode == 1 ? t->col + 1 : cols);
        break;
    case 'm':
        set_graphics();
        break;
    }
}

static void put_text(char c) {
    dirty[t->row] = true;
    switch (c) {
    case '\n':
        new_line();
        return;
    case '\r':
        t->col = 0;
        return;
    case '\b':
        if (t->col > 0)
            t->col--;
        return;
    case '\t':
        do {
            set_cell(t->row, t->col, ' ');
            if (++t->col == cols)
                new_line();
        } while (t->col % TAB_WIDTH != 0);
        return;
    case '\a':
        return;
    case '\x1b':
        t->state = PARSE_ESCAPE;
        return;
    }

    // Other control characters and non-ASCII bytes have no glyph here.
    set_cell(t->row, t->col, (uint8_t)c >= 0x20 && (uint8_t)c < 0x7F ? c : '?');
    if (++t->col == cols)
        new_line();
}

static void put_byte(char c) {
    switch (t->state) {
    case PARSE_TEXT:
        put_text(c);
        break;
    case PARSE_ESCAPE:
        t->state = c == '[' ? PARSE_CSI : PARSE_TEXT;
        t->param_count = 0;
        t->private_sequence = false;
        memset(t->params, 0, sizeof(t->params));
        break;
    case PARSE_CSI:
        if (c == '?') {
            t->private_sequence = true;
        } else if (c >= '0' && c <= '9') {
            if (t->param_count == 0)
                t->param_count = 1;
            t->params[t->param_count - 1] = t->params[t->param_count - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (t->param_count == 0)
                t->param_count = 1;
            if (t->param_count < MAX_PARAMS)
                t->param_count++;
        } else {
            run_csi(c);
            t->state = PARSE_TEXT;
        }
        break;
    }
}

// A line counting from the oldest kept, through the scrollback and onto the screen.
static const struct cell *line_cells(int line) {
    return line < t->history_count ? history_line(line) : t->cells[line - t->history_count];
}

// What's shown on row r of the window, which is in the scrollback when scrolled back.
static int shown_line(int r) {
    return t->history_count - t->view + r;
}

static long order(struct place p) {
    return (long)p.line * MAX_COLS + p.col;
}

static bool is_selected(int line, int c) {
    long from = order(t->select_from), to = order(t->select_to), at = order((struct place){line, c});

    return t->selected && at >= (from < to ? from : to) && at <= (from < to ? to : from);
}

static void draw_cell(int r, int c) {
    struct cell cell = line_cells(shown_line(r))[c];
    bool cursor = r - t->view == t->row && c == t->col && !t->cursor_hidden;
    uint32_t fg = palette[cell.foreground], bg = palette[cell.background];
    int x = PADDING + c * CELL_WIDTH, y = top + r * CELL_HEIGHT;

    // A solid block when focused, an outline when not, and what's selected reversed.
    if ((cursor && focused) != is_selected(shown_line(r), c)) {
        uint32_t swap = fg;
        fg = bg;
        bg = swap;
    }
    gui_fill(&window->canvas, x, y, CELL_WIDTH, CELL_HEIGHT, bg);
    gui_text(&window->canvas, x, y, (char[]){(char)(cell.c & 0x7F), '\0'}, fg, 1);
    if (cursor && !focused) {
        gui_fill(&window->canvas, x, y, CELL_WIDTH, 1, fg);
        gui_fill(&window->canvas, x, y + CELL_HEIGHT - 1, CELL_WIDTH, 1, fg);
        gui_fill(&window->canvas, x, y, 1, CELL_HEIGHT, fg);
        gui_fill(&window->canvas, x + CELL_WIDTH - 1, y, 1, CELL_HEIGHT, fg);
    }
}

// A thumb in the right margin while scrolled back, showing where.
static void draw_scrollbar(void) {
    int x = window->canvas.width - PADDING, height = window->canvas.height - top + PADDING, total = t->history_count + rows;

    gui_fill(&window->canvas, x, top - PADDING, PADDING, height, palette[DEFAULT_BACKGROUND]);
    if (t->view)
        gui_fill(&window->canvas, x + 1, top - PADDING + (t->history_count - t->view) * height / total, PADDING - 2,
                 rows * height / total, palette[BRIGHT]);
    gui_update(window, x, top - PADDING, PADDING, height);
}

static uint32_t panel_colour(void) {
    return (palette[DEFAULT_BACKGROUND] & 0xFFFFFF) == 0x161A22 ? 0x2B303B : 0xE2E5EB;
}

static void box_text(int x, int y, const char *s, uint32_t colour) {
    gui_text(&window->canvas, x, y, s, colour, 1);
}

// The tabs, across the top when there's more than one, each with an x to close it.
static int tab_width(void) {
    int w = (window->canvas.width - 8) / (session_count ? session_count : 1);

    return w < TAB_WIDTH_MAX ? w : TAB_WIDTH_MAX;
}

static void draw_tabs(void) {
    struct gui_canvas *c = &window->canvas;
    int w = tab_width();

    if (session_count < 2)
        return;
    gui_fill(c, 0, 0, c->width, TAB_BAR, panel_colour());
    for (int i = 0; i < session_count; i++) {
        char name[32];

        snprintf(name, sizeof(name), "Shell %d", sessions[i]->number);
        if (i == active)
            gui_fill(c, 4 + i * w, 4, w - 2, TAB_BAR - 4, palette[DEFAULT_BACKGROUND]);
        box_text(4 + i * w + 8, 6, name, i == active ? palette[DEFAULT_FOREGROUND] : palette[BRIGHT]);
        box_text(4 + (i + 1) * w - 18, 6, "x", palette[BRIGHT]);
    }
    gui_update(window, 0, 0, c->width, TAB_BAR);
}

// The search bar, over the bottom of the window.
static void draw_search(void) {
    struct gui_canvas *c = &window->canvas;
    char line[160];
    int y = c->height - CELL_HEIGHT - 8;

    snprintf(line, sizeof(line), "Find: %s_  %s", query, search_status);
    gui_fill(c, 0, y, c->width, CELL_HEIGHT + 8, panel_colour());
    box_text(PADDING + 4, y + 4, line, palette[DEFAULT_FOREGROUND]);
    gui_update(window, 0, y, c->width, CELL_HEIGHT + 8);
}

static void draw_menu(void) {
    gui_fill(&window->canvas, menu_x, menu_y, MENU_WIDTH, MENU_ITEMS * MENU_ROW + 8, panel_colour());
    for (int i = 0; i < MENU_ITEMS; i++)
        box_text(menu_x + 10, menu_y + 4 + i * MENU_ROW + 3, menu_items[i], palette[DEFAULT_FOREGROUND]);
    gui_update(window, menu_x, menu_y, MENU_WIDTH, MENU_ITEMS * MENU_ROW + 8);
}

static void draw_help(void) {
    int w = 40 * CELL_WIDTH + 32, h = HELP_LINES * CELL_HEIGHT + 32;
    int x = (window->canvas.width - w) / 2, y = (window->canvas.height - h) / 2;

    gui_fill(&window->canvas, x, y, w, h, panel_colour());
    for (int i = 0; i < HELP_LINES; i++)
        box_text(x + 16, y + 16 + i * CELL_HEIGHT, help[i], palette[DEFAULT_FOREGROUND]);
    gui_update(window, x, y, w, h);
}

// What goes over the cells, drawn again after them each time.
static void draw_over(void) {
    if (searching)
        draw_search();
    if (help_shown)
        draw_help();
    if (menu_open)
        draw_menu();
}

static void draw(void) {
    static int drawn_view;
    int first = rows, last = -1;

    // Changes on the screen move about the window while scrolled back.
    for (int r = 0; t->view && r < rows; r++) {
        if (dirty[r])
            mark_all_dirty();
    }
    if (t->view != drawn_view || (t->view && dirty[0]))
        draw_scrollbar();
    drawn_view = t->view;
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
        gui_update(window, PADDING, top + first * CELL_HEIGHT, cols * CELL_WIDTH, (last - first + 1) * CELL_HEIGHT);
    draw_over();
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
    case KEY_RIGHT: sequence = key->modifiers & KEY_MOD_CTRL ? "\x1b[1;5C" : "\x1b[C"; break;
    case KEY_LEFT: sequence = key->modifiers & KEY_MOD_CTRL ? "\x1b[1;5D" : "\x1b[D"; break;
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

static void use_theme(void) {
    struct gui_settings settings;

    gui_settings_load(&settings);
    palette[DEFAULT_FOREGROUND] = settings.light ? 0x1E2129 : 0xD8DEE9;
    palette[DEFAULT_BACKGROUND] = SEE_THROUGH | (settings.light ? 0xFAFAFA : 0x161A22);
}

static void paint_background(void);

static void scroll_view(int lines) {
    t->view = clamp(t->view + lines, 0, t->history_count);
    mark_all_dirty();
}

// The cell under a point in the window.
static struct place place_at(int x, int y) {
    return (struct place){shown_line(clamp((y - top) / CELL_HEIGHT, 0, rows - 1)),
                          clamp((x - PADDING) / CELL_WIDTH, 0, cols - 1)};
}

static long now_ms(void);

// Selects the word at a place: the run of characters there that aren't spaces.
static void select_word(struct place p) {
    const struct cell *line = line_cells(p.line);
    int from = p.col, to = p.col;

    while (from > 0 && line[from - 1].c != ' ')
        from--;
    while (to < cols - 1 && line[to + 1].c != ' ')
        to++;
    t->select_from = (struct place){p.line, from};
    t->select_to = (struct place){p.line, to};
    t->selected = line[p.col].c != ' ';
}

// The mouse selects by dragging, or a word with a double click. A click on its own
// selects nothing.
static void mouse(const struct gui_message *e) {
    if (e->type == GUI_MOUSE_BUTTON && (e->changed & 1)) {
        selecting = e->buttons & 1;
        if (selecting && now_ms() - last_press < 400) {
            select_word(place_at(e->x, e->y));
            selecting = false;
            last_press = 0;
        } else if (selecting) {
            t->select_from = t->select_to = place_at(e->x, e->y);
            t->selected = false;
            last_press = now_ms();
        }
    } else if (e->type == GUI_MOUSE_MOVE && selecting) {
        t->select_to = place_at(e->x, e->y);
        t->selected = order(t->select_from) != order(t->select_to);
    }
    drag_x = e->x;
    drag_y = e->y;
    mark_all_dirty();
}

// How many lines back to scroll while a selection is dragged above the window,
// or forward when below it, more the further out it is.
static int drag_lines(void) {
    int bottom = top + rows * CELL_HEIGHT;

    if (!selecting)
        return 0;
    if (drag_y < top)
        return 1 + (top - drag_y) / CELL_HEIGHT;
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
        t->select_to = place_at(drag_x, drag_y);
        t->selected = order(t->select_from) != order(t->select_to);
    }
    return (int)(DRAG_SCROLL_MS - (now - last));
}

// Each line's text, without the spaces at its end, with a line break between them.
static void copy_selection(void) {
    struct place from = order(t->select_from) < order(t->select_to) ? t->select_from : t->select_to;
    struct place to = order(t->select_from) < order(t->select_to) ? t->select_to : t->select_from;
    char *text = t->selected ? malloc((size_t)(to.line - from.line + 1) * (MAX_COLS + 1)) : NULL;
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

    if (!text)
        return;
    for (size_t i = 0; i < length; i++)
        text[i] = text[i] == '\n' ? '\r' : text[i];
    write_all(master, text, length);
    free(text);
}

// Fits as many cells as the new size holds, keeping the cursor's line on the
// screen, and tells the programs on the terminal.
static void fit(struct session *s, int new_cols, int new_rows) {
    int lost = s->row - (new_rows - 1);

    t = s;
    if (lost > 0) {
        for (int r = 0; r < lost; r++)
            keep_line(s->cells[r]);
        memmove(s->cells[0], s->cells[lost], (rows - lost) * sizeof(s->cells[0]));
        s->row -= lost;
    }

    // What comes into view is blank.
    int kept = lost > 0 ? rows - lost : rows;

    for (int r = 0; r < new_rows; r++) {
        for (int c = r < kept ? cols : 0; c < new_cols; c++)
            s->cells[r][c] = (struct cell){' ', DEFAULT_FOREGROUND, DEFAULT_BACKGROUND};
    }
    s->col = clamp(s->col, 0, new_cols - 1);
    s->view = clamp(s->view, 0, s->history_count);

    struct winsize size = {.ws_row = new_rows, .ws_col = new_cols, .ws_xpixel = new_cols * CELL_WIDTH,
                           .ws_ypixel = new_rows * CELL_HEIGHT};

    ioctl(s->master, TIOCSWINSZ, &size);
}

// Lays the window out again: the tab bar if there's more than one tab, and every
// terminal fitted under it.
static void layout(void) {
    int width = window->canvas.width, height = window->canvas.height;

    top = PADDING + (session_count > 1 ? TAB_BAR : 0);

    int new_cols = clamp((width - 2 * PADDING) / CELL_WIDTH, 1, MAX_COLS);
    int new_rows = clamp((height - top - PADDING) / CELL_HEIGHT, 1, MAX_ROWS);

    if (new_cols != cols || new_rows != rows) {
        for (int i = 0; i < session_count; i++)
            fit(sessions[i], new_cols, new_rows);
        cols = new_cols;
        rows = new_rows;
    }
    t = sessions[active];
    paint_background();
    draw();
    draw_scrollbar();
    gui_show(window);
}

static void resize(int width, int height) {
    if (gui_resize(window, width, height))
        layout();
}

static void paint_background(void) {
    gui_fill(&window->canvas, 0, 0, window->canvas.width, window->canvas.height, palette[DEFAULT_BACKGROUND]);
    draw_tabs();
    mark_all_dirty();
}

static bool new_session(char **command) {
    struct winsize size = {.ws_row = rows, .ws_col = cols, .ws_xpixel = cols * CELL_WIDTH, .ws_ypixel = rows * CELL_HEIGHT};
    static int opened;
    struct session *s;
    int slave;

    if (session_count == MAX_TABS || !(s = calloc(1, sizeof(*s))))
        return false;
    if (openpty(&s->master, &slave, NULL, NULL, &size) < 0) {
        free(s);
        return false;
    }
    s->child = fork();
    if (s->child == 0) {
        close(s->master);
        close(gui_fd());
        for (int i = 0; i < session_count; i++)
            close(sessions[i]->master);
        login_tty(slave);
        if (command && *command)
            execvp(command[0], command);
        else
            execl("/system/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    close(slave);
    s->number = ++opened;
    s->foreground = DEFAULT_FOREGROUND;
    s->background = DEFAULT_BACKGROUND;
    t = s;
    for (int r = 0; r < MAX_ROWS; r++)
        clear_cells(r, 0, MAX_COLS);
    sessions[session_count++] = s;
    active = session_count - 1;
    printf("term: tab %d of %d\n", active + 1, session_count);
    fflush(stdout);
    if (window)
        layout();
    return true;
}

static void switch_to(int index) {
    active = (index + session_count) % session_count;
    t = sessions[active];
    searching = false;
    printf("term: tab %d of %d\n", active + 1, session_count);
    fflush(stdout);
    paint_background();
    draw();
    draw_scrollbar();
    gui_show(window);
}

// Closing the master hangs up whatever is running on the terminal.
static void close_session(int index) {
    close(sessions[index]->master);
    kill(sessions[index]->child, SIGHUP);
    free(sessions[index]);
    memmove(sessions + index, sessions + index + 1, (session_count - index - 1) * sizeof(*sessions));
    session_count--;
    if (!session_count)
        return;
    active = active > index || active == session_count ? active - 1 : active;
    printf("term: tab %d of %d\n", active + 1, session_count);
    fflush(stdout);
    layout();
}

// Searches what's kept for the query, from the line before or after the last found,
// going older or newer, and shows it selected.
static void find(bool older) {
    int total = t->history_count + rows, step = older ? -1 : 1;
    int from = found_line >= 0 && found_line < total ? found_line + step : older ? total - 1 : 0;
    size_t length = strlen(query);

    for (int line = from; length && line >= 0 && line < total; line += step) {
        const struct cell *cells_of = line_cells(line);

        for (int c = 0; c + (int)length <= cols; c++) {
            size_t i = 0;

            while (i < length && tolower((unsigned char)cells_of[c + i].c) == tolower((unsigned char)query[i]))
                i++;
            if (i < length)
                continue;
            found_line = line;
            t->select_from = (struct place){line, c};
            t->select_to = (struct place){line, c + (int)length - 1};
            t->selected = true;
            t->view = clamp(t->history_count + rows / 2 - line, 0, t->history_count);
            snprintf(search_status, sizeof(search_status), "line %d", line + 1);
            printf("term: found %s on line %d\n", query, line + 1);
            fflush(stdout);
            mark_all_dirty();
            draw_scrollbar();
            return;
        }
    }
    snprintf(search_status, sizeof(search_status), length ? "not found" : "");
    printf("term: %s not found\n", query);
    fflush(stdout);
}

// Keys while searching type what to find.
static void search_key(const struct key_event *k) {
    size_t length = strlen(query);

    if (k->code == KEY_ESCAPE) {
        searching = false;
        paint_background();
        return;
    }
    if (k->code == KEY_ENTER)
        find(!(k->modifiers & KEY_MOD_SHIFT));
    else if (k->code == KEY_BACKSPACE && length)
        query[length - 1] = '\0';
    else if (k->text >= ' ' && k->text < 127 && length < sizeof(query) - 1 && !(k->modifiers & KEY_MOD_CTRL))
        query[length] = (char)k->text, query[length + 1] = '\0';
    if (k->code != KEY_ENTER)
        found_line = -1;
}

static void start_search(void) {
    searching = true;
    query[0] = search_status[0] = '\0';
    found_line = -1;
}

static void close_over(void) {
    menu_open = help_shown = false;
    paint_background();
}

static void choose(int item) {
    close_over();
    if (item == 0) {
        copy_selection();
    } else if (item == 1) {
        if (t->view)
            scroll_view(-t->view);
        paste(t->master);
    } else if (item == 2) {
        new_session(NULL);
    } else if (item == 3) {
        start_search();
    } else if (item == 4) {
        help_shown = true;
    }
}

// The mouse on the tabs, a menu or the help, returning whether it was theirs.
static bool mouse_over(const struct gui_message *e) {
    bool pressed = e->type == GUI_MOUSE_BUTTON && (e->buttons & ~e->changed) == 0 && e->buttons;
    bool right = pressed && (e->changed & 2);

    if (menu_open && pressed) {
        int item = (e->y - menu_y - 4) / MENU_ROW;

        if (e->x >= menu_x && e->x < menu_x + MENU_WIDTH && e->y >= menu_y + 4 && item < MENU_ITEMS && !right)
            choose(item);
        else
            close_over();
        if (!right)
            return true;
    }
    if (help_shown && pressed) {
        close_over();
        return true;
    }
    if (right) {
        menu_x = clamp(e->x, 0, window->canvas.width - MENU_WIDTH);
        menu_y = clamp(e->y, 0, window->canvas.height - MENU_ITEMS * MENU_ROW - 8);
        menu_open = true;
        return true;
    }
    if (pressed && session_count > 1 && e->y < TAB_BAR) {
        int i = (e->x - 4) / tab_width();

        if (i >= 0 && i < session_count && e->x >= 4 + (i + 1) * tab_width() - 22)
            close_session(i);
        else if (i >= 0 && i < session_count)
            switch_to(i);
        return true;
    }
    return e->type == GUI_MOUSE_BUTTON && e->y < top - PADDING && !selecting;
}

// Keys for the window rather than the shell: copying and pasting, tabs and finding.
static bool window_key(const struct key_event *k) {
    bool shifted = k->modifiers & KEY_MOD_SHIFT, ctrl = k->modifiers & KEY_MOD_CTRL;

    if (help_shown || menu_open) {
        if (k->code == KEY_ESCAPE)
            close_over();
        return true;
    }
    if (searching) {
        search_key(k);
        return true;
    }
    if (ctrl && shifted && k->code == KEY_C)
        copy_selection();
    else if (ctrl && shifted && k->code == KEY_V)
        choose(1);
    else if (ctrl && shifted && k->code == KEY_T)
        new_session(NULL);
    else if (ctrl && shifted && k->code == KEY_W)
        close_session(active);
    else if (ctrl && shifted && k->code == KEY_F)
        start_search();
    else if (ctrl && (k->code == KEY_TAB || k->code == KEY_PAGE_DOWN || k->code == KEY_PAGE_UP))
        switch_to(active + (k->code == KEY_PAGE_UP || (k->code == KEY_TAB && shifted) ? -1 : 1));
    else if (shifted && (k->code == KEY_PAGE_UP || k->code == KEY_PAGE_DOWN))
        scroll_view((k->code == KEY_PAGE_UP ? 1 : -1) * (rows > 1 ? rows - 1 : 1));
    else
        return false;
    return true;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: term [command [argument]...]\n"
                            "Opens a terminal window running the shell, or the command.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "term: wm isn't running\n");
        return 1;
    }
    if (!new_session(argv + 1)) {
        dprintf(STDERR_FILENO, "term: %s\n", strerror(errno));
        return 1;
    }
    window = gui_create_window("Terminal", cols * CELL_WIDTH + 2 * PADDING, rows * CELL_HEIGHT + 2 * PADDING);
    if (!window) {
        dprintf(STDERR_FILENO, "term: couldn't make a window\n");
        return 1;
    }
    gui_set_cursor(window, GUI_CURSOR_TEXT);
    gui_set_translucent(window, true);
    use_theme();
    layout();

    while (session_count) {
        struct pollfd fds[1 + MAX_TABS] = {{.fd = gui_fd(), .events = POLLIN}};
        char buf[4096];
        int count = session_count;

        for (int i = 0; i < count; i++)
            fds[1 + i] = (struct pollfd){.fd = sessions[i]->master, .events = POLLIN};

        int timeout = drag_scroll();

        draw(); // what came last time round, and any scrolling
        if (poll(fds, 1 + count, timeout) < 0 && errno != EINTR)
            break;

        // Each shell's output, and its tab closing when everything on its terminal has exited.
        for (int i = count - 1; i >= 0; i--) {
            if (!(fds[1 + i].revents & (POLLIN | POLLHUP)))
                continue;

            ssize_t n = read(sessions[i]->master, buf, sizeof(buf));

            t = sessions[i];
            for (ssize_t j = 0; j < n; j++)
                put_byte(buf[j]);
            if (i == active)
                dirty[t->row] = true; // where the cursor is now
            t = sessions[active];
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                close_session(i);
                if (!session_count)
                    break;
            }
        }
        if (!session_count)
            break;

        struct gui_message e;

        if ((fds[0].revents & (POLLIN | POLLHUP)) && gui_next_event(&e)) {
            // Keys go to the shell, taking the view back to the bottom, unless
            // they're the window's.
            if (e.type == GUI_KEY && e.key.pressed && !window_key(&e.key)) {
                if (send_key(t->master, &e.key) && t->view)
                    scroll_view(-t->view);
            } else if (e.type == GUI_MOUSE_MOVE || e.type == GUI_MOUSE_BUTTON) {
                if (!mouse_over(&e))
                    mouse(&e);
            } else if (e.type == GUI_MOUSE_WHEEL) {
                scroll_view(-e.wheel * WHEEL_LINES);
            } else if (e.type == GUI_FOCUS) {
                focused = e.focused;
                dirty[t->row] = true;
            } else if (e.type == GUI_RESIZE) {
                resize(e.width, e.height);
            } else if (e.type == GUI_SETTINGS) {
                use_theme();
                layout();
            } else if (e.type == GUI_CLOSE) {
                break;
            }
        } else if (fds[0].revents & (POLLIN | POLLHUP)) {
            break; // wm has gone
        }
    }
    while (session_count)
        close_session(session_count - 1);
    gui_destroy_window(window);
    return 0;
}
