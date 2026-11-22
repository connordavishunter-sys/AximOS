#include <console.h>
#include <font8x8_basic.h>
#include <serial.h>
#include <stdint.h>
#include <string.h>

// Each 8x8 glyph is drawn at twice its size.
#define SCALE 2
#define CELL_SIZE (8 * SCALE)
#define MAX_ROWS 200
#define MAX_COLS 320
#define DEFAULT_ROWS 25
#define DEFAULT_COLS 80
#define TAB_WIDTH 8
#define MAX_PARAMS 8
#define PARAM_HIDE_CURSOR 25

// A cell is a 7 bit glyph, a bit for reverse video and two colours. Zero is a blank
// in the default colours, which is why the foreground is stored inverted.
#define CELL_GLYPH 0x7F
#define CELL_REVERSE 0x80
#define CELL_FOREGROUND_SHIFT 8
#define CELL_BACKGROUND_SHIFT 12
#define COLOR_MASK 0xF
#define DEFAULT_FOREGROUND 15
#define DEFAULT_BACKGROUND 0
#define BRIGHT 8

static const uint32_t palette[16] = {
    0x00000000, 0x00CC3333, 0x0033AA33, 0x00CCAA33, 0x003366CC, 0x00AA44AA, 0x0033AAAA, 0x00C0C0C0,
    0x00666666, 0x00FF6666, 0x0066DD66, 0x00FFDD66, 0x006699FF, 0x00DD77DD, 0x0066DDDD, 0xFFFFFFFF,
};

enum parser_state {
    PARSE_TEXT,
    PARSE_ESCAPE,
    PARSE_CSI,
};

static uint8_t *fb_base;
static uint64_t fb_pitch;
static uint64_t fb_height;
static bool hidden; // a program owns the screen, so text only reaches cells
static int rows = DEFAULT_ROWS;
static int cols = DEFAULT_COLS;
static int row;
static int col;

// Kept so a cell can be redrawn when the cursor leaves it.
static uint16_t cells[MAX_ROWS][MAX_COLS];
static uint16_t attribute;
static bool bold;
static bool cursor_hidden;
static bool private_sequence;

static enum parser_state state;
static int params[MAX_PARAMS];
static int param_count;

static void draw_cell(int r, int c, bool inverted);

void console_set_base(uint8_t *base) {
    fb_base = base;
}

void console_set_screen(uint8_t *base, uint64_t width, uint64_t height, uint64_t pitch) {
    fb_base = base;
    fb_pitch = pitch;
    fb_height = height;
    rows = height / CELL_SIZE;
    cols = width / CELL_SIZE;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    row = col = 0;
    memset(cells, 0, sizeof(cells));
    kprintf("console: %lux%lu framebuffer, %u rows, %u cols\n", width, height, rows, cols);
}

void console_hide(void) {
    draw_cell(row, col, false);
    hidden = true;
}

void console_show(void) {
    hidden = false;
    if (!fb_base)
        return;

    memset(fb_base, 0, fb_height * fb_pitch);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++)
            draw_cell(r, c, false);
    }
    draw_cell(row, col, !cursor_hidden);
}

void console_size(int *out_rows, int *out_cols) {
    *out_rows = rows;
    *out_cols = cols;
}

static void draw_cell(int r, int c, bool inverted) {
    if (!fb_base || hidden)
        return;

    uint16_t cell = cells[r][c];

    inverted ^= (cell & CELL_REVERSE) != 0;

    uint32_t front = palette[DEFAULT_FOREGROUND - (cell >> CELL_FOREGROUND_SHIFT & COLOR_MASK)];
    uint32_t back = palette[cell >> CELL_BACKGROUND_SHIFT & COLOR_MASK];
    uint32_t foreground = inverted ? back : front;
    uint32_t background = inverted ? front : back;
    uint8_t glyph_index = cell & CELL_GLYPH;
    const char *glyph = font8x8_basic[glyph_index];

    for (int y = 0; y < CELL_SIZE; y++) {
        uint32_t *line = (uint32_t *)(fb_base + (r * CELL_SIZE + y) * fb_pitch) + c * CELL_SIZE;
        uint8_t bits = glyph[y / SCALE];

        for (int x = 0; x < CELL_SIZE; x++)
            line[x] = (bits >> (x / SCALE) & 1) ? foreground : background;
    }
}

static void set_cell(int r, int c, char value) {
    cells[r][c] = (uint8_t)value | attribute;
    draw_cell(r, c, false);
}

static void clear_cells(int r, int from, int to) {
    for (int c = from; c < to; c++)
        set_cell(r, c, 0);
}

static void scroll(void) {
    memcpy(cells[0], cells[1], sizeof(cells[0]) * (rows - 1));
    memset(cells[rows - 1], 0, sizeof(cells[0]));

    if (fb_base && !hidden) {
        uint64_t row_bytes = CELL_SIZE * fb_pitch;

        memcpy(fb_base, fb_base + row_bytes, row_bytes * (rows - 1));
        memset(fb_base + row_bytes * (rows - 1), 0, row_bytes);
    }
}

static void new_line(void) {
    col = 0;
    if (++row == rows) {
        scroll();
        row = rows - 1;
    }
}

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static int param_or_one(int index) {
    return index < param_count && params[index] > 0 ? params[index] : 1;
}

static void erase_display(int mode) {
    if (mode == 2) {
        for (int r = 0; r < rows; r++)
            clear_cells(r, 0, cols);
        return;
    }
    clear_cells(row, col, cols);
    for (int r = row + 1; r < rows; r++)
        clear_cells(r, 0, cols);
}

static void set_foreground(int color) {
    attribute &= ~(COLOR_MASK << CELL_FOREGROUND_SHIFT);
    attribute |= (DEFAULT_FOREGROUND - color) << CELL_FOREGROUND_SHIFT;
}

static void set_background(int color) {
    attribute &= ~(COLOR_MASK << CELL_BACKGROUND_SHIFT);
    attribute |= color << CELL_BACKGROUND_SHIFT;
}

// Bold shows as the bright version, as the font has no heavier one.
static void set_graphics(void) {
    for (int i = 0; i < (param_count ? param_count : 1); i++) {
        int p = params[i];

        if (p == 0) {
            attribute = 0;
            bold = false;
        } else if (p == 1) {
            bold = true;
        } else if (p == 22) {
            bold = false;
        } else if (p == 7) {
            attribute |= CELL_REVERSE;
        } else if (p == 27) {
            attribute &= ~CELL_REVERSE;
        } else if (p >= 30 && p <= 37) {
            set_foreground(p - 30 + (bold ? BRIGHT : 0));
        } else if (p == 39) {
            set_foreground(DEFAULT_FOREGROUND);
        } else if (p >= 40 && p <= 47) {
            set_background(p - 40);
        } else if (p == 49) {
            set_background(DEFAULT_BACKGROUND);
        } else if (p >= 90 && p <= 97) {
            set_foreground(p - 90 + BRIGHT);
        } else if (p >= 100 && p <= 107) {
            set_background(p - 100 + BRIGHT);
        } else if (p == 38 || p == 48) {
            return; // what follows says which of the many colours there's no room for
        }
    }
}

static void run_csi(char final) {
    int mode = param_count > 0 ? params[0] : 0;

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
    case 'H':
        row = clamp(param_or_one(0) - 1, 0, rows - 1);
        col = clamp(param_or_one(1) - 1, 0, cols - 1);
        break;
    case 'J':
        erase_display(mode);
        break;
    case 'K':
        clear_cells(row, mode == 2 ? 0 : col, cols);
        break;
    case 'm':
        set_graphics();
        break;
    }
}

static void put_text(char c) {
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
    case '\x1b':
        state = PARSE_ESCAPE;
        return;
    }

    // Other control characters and non-ASCII bytes have no glyph here.
    bool printable = (uint8_t)c >= 0x20 && (uint8_t)c < 0x7F;
    set_cell(row, col, printable ? c : '?');
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

void console_write(const char *buf, size_t len) {
    serial_write(buf, len);
    console_draw(buf, len);
}

void console_draw(const char *buf, size_t len) {
    draw_cell(row, col, false);
    for (size_t i = 0; i < len; i++)
        put_byte(buf[i]);
    draw_cell(row, col, !cursor_hidden);
}
