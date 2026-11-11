#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <mouse_event.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define MAX_WINDOWS 64
#define MAX_DAMAGE 32
#define MAX_SIZE 4096
#define INBOX_SIZE (8 * sizeof(struct gui_message))
#define OUTBOX_SIZE (64 * sizeof(struct gui_message))

#define TITLE_HEIGHT 24
#define BORDER 1
#define BUTTON_SIZE 16
#define CASCADE 32
#define RESIZE_MARGIN 6 // past the right and bottom edges, where dragging resizes
#define MIN_WIDTH 120
#define MIN_HEIGHT 48

#define TASKBAR_HEIGHT 28
#define TASKBAR_BUTTON_WIDTH 160
#define LAUNCHER_WIDTH 96

#define DESKTOP_COLOR 0x1E2430
#define HINT_COLOR 0x5C677D
#define BORDER_COLOR 0x0E1118
#define TITLE_FOCUSED 0x3D63DD
#define TITLE_UNFOCUSED 0x3A4150
#define TITLE_TEXT 0xFFFFFF
#define CLOSE_COLOR 0xE5484D
#define BUTTON_COLOR 0x56607A
#define TASKBAR_COLOR 0x141821
#define TASKBAR_LINE 0x2A3140
#define TASKBAR_BUTTON 0x2E3646
#define TASKBAR_MINIMIZED 0x1D222C
#define TASKBAR_DIM_TEXT 0x8A93A6

// Super+Enter runs this.
#define LAUNCH_COMMAND "term"
#define HINT "Super+Enter: terminal   Super+Q: close   Super+arrows: tile   Super+Shift+Q: quit"

struct rect {
    int x, y, width, height;
};

struct client {
    int fd;
    char inbox[INBOX_SIZE];
    size_t in_used;
    char outbox[OUTBOX_SIZE];
    size_t out_used;
    bool stuck; // stopped reading, with something that can't be dropped waiting
};

// x and y are where the contents start, inside the frame.
struct window {
    struct client *client;
    uint32_t id;
    int x, y;
    char title[GUI_TITLE_MAX];
    struct gui_canvas canvas;
    bool minimized;

    // The size it's been asked to be. One request is out at a time, and the program
    // answers it with a new buffer of whatever size it chose.
    int want_width, want_height;
    int asked_width, asked_height;
    bool asking;

    // Where it was before being maximized or tiled, to go back to.
    bool tiled;
    struct rect restore;
};

enum part { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE, PART_RESIZE };
enum { EDGE_RIGHT = 1, EDGE_BOTTOM = 2 };

static struct gui_canvas screen; // composed here, then copied to the framebuffer
static uint8_t *framebuffer;
static int pitch;

static struct client *clients[MAX_CLIENTS];
static struct window *windows[MAX_WINDOWS]; // bottom first
static int window_count;
static uint32_t next_id = 1;
static struct window *focused;

static struct rect damage[MAX_DAMAGE];
static int damage_count;

static int cursor_x, cursor_y;
static uint32_t buttons;
static struct window *dragging; // by its title bar
static int drag_x, drag_y;
static struct window *grabbed; // pressed in its contents, so it gets the mouse until release
static struct window *resizing;
static int resize_edges, resize_x, resize_y, resize_width, resize_height;
static bool quitting;

static const char *const cursor[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
#define CURSOR_WIDTH 12
#define CURSOR_HEIGHT ((int)(sizeof(cursor) / sizeof(cursor[0])))

static struct rect frame_of(const struct window *w) {
    return (struct rect){w->x - BORDER, w->y - TITLE_HEIGHT - BORDER, w->canvas.width + 2 * BORDER,
                         w->canvas.height + TITLE_HEIGHT + 2 * BORDER};
}

// The title bar's buttons from the right: close, maximize, minimize.
static struct rect button_of(const struct window *w, int index) {
    int gap = (TITLE_HEIGHT - BUTTON_SIZE) / 2;

    return (struct rect){w->x + w->canvas.width - (index + 1) * (BUTTON_SIZE + gap),
                         w->y - TITLE_HEIGHT + gap, BUTTON_SIZE, BUTTON_SIZE};
}

// Where windows go: everything above the taskbar.
static struct rect desktop(void) {
    return (struct rect){0, 0, screen.width, screen.height - TASKBAR_HEIGHT};
}

static struct rect taskbar(void) {
    return (struct rect){0, screen.height - TASKBAR_HEIGHT, screen.width, TASKBAR_HEIGHT};
}

static bool contains(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

static struct rect bounds(struct rect a, struct rect b) {
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int right = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;

    return (struct rect){x, y, right - x, bottom - y};
}

// Clipped to the screen. When the list is full, everything merges into one.
static void add_damage(struct rect r) {
    if (r.x < 0) {
        r.width += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.height += r.y;
        r.y = 0;
    }
    if (r.width > screen.width - r.x)
        r.width = screen.width - r.x;
    if (r.height > screen.height - r.y)
        r.height = screen.height - r.y;
    if (r.width <= 0 || r.height <= 0)
        return;

    if (damage_count == MAX_DAMAGE) {
        for (int i = 1; i < damage_count; i++)
            damage[0] = bounds(damage[0], damage[i]);
        damage_count = 1;
        damage[0] = bounds(damage[0], r);
        return;
    }
    damage[damage_count++] = r;
}

static void damage_title(const struct window *w) {
    if (w)
        add_damage((struct rect){w->x, w->y - TITLE_HEIGHT, w->canvas.width, TITLE_HEIGHT});
}

static void damage_taskbar(void) {
    add_damage(taskbar());
}

static void damage_cursor(void) {
    add_damage((struct rect){cursor_x, cursor_y, CURSOR_WIDTH, CURSOR_HEIGHT});
}

static void draw_window(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    struct rect frame = frame_of(w), close = button_of(w, 0), maximize = button_of(w, 1), minimize = button_of(w, 2);
    uint32_t title_color = w == focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
    int text_y = w->y - TITLE_HEIGHT + (TITLE_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;

    gui_fill(c, frame.x - ox, frame.y - oy, frame.width, frame.height, BORDER_COLOR);
    gui_fill(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT, title_color);
    gui_text(c, w->x + 8 - ox, text_y, w->title, TITLE_TEXT, 1);

    // A title too long for the bar goes under the buttons.
    gui_fill(c, minimize.x - 4 - ox, w->y - TITLE_HEIGHT - oy, w->x + w->canvas.width - minimize.x + 4, TITLE_HEIGHT,
             title_color);
    gui_fill(c, close.x - ox, close.y - oy, BUTTON_SIZE, BUTTON_SIZE, CLOSE_COLOR);
    gui_text(c, close.x + (BUTTON_SIZE - GUI_CHAR_WIDTH) / 2 - ox, text_y, "x", TITLE_TEXT, 1);
    gui_fill(c, maximize.x - ox, maximize.y - oy, BUTTON_SIZE, BUTTON_SIZE, BUTTON_COLOR);
    gui_fill(c, maximize.x + 4 - ox, maximize.y + 4 - oy, 8, 8, TITLE_TEXT);
    gui_fill(c, maximize.x + 5 - ox, maximize.y + 6 - oy, 6, 5, BUTTON_COLOR);
    gui_fill(c, minimize.x - ox, minimize.y - oy, BUTTON_SIZE, BUTTON_SIZE, BUTTON_COLOR);
    gui_fill(c, minimize.x + 4 - ox, minimize.y + 11 - oy, 8, 2, TITLE_TEXT);
    gui_copy(c, w->x - ox, w->y - oy, &w->canvas, 0, 0, w->canvas.width, w->canvas.height);
}

// Windows in the order they were made, which is how the taskbar shows them.
static int windows_by_age(struct window *out[MAX_WINDOWS]) {
    int count = 0;

    for (int i = 0; i < window_count; i++) {
        int j = count++;

        while (j > 0 && out[j - 1]->id > windows[i]->id) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = windows[i];
    }
    return count;
}

static struct rect taskbar_button(int index, int count) {
    int start = LAUNCHER_WIDTH + 12, room = screen.width - start - 64;
    int width = count && room / count < TASKBAR_BUTTON_WIDTH ? room / count : TASKBAR_BUTTON_WIDTH;

    return (struct rect){start + index * width, taskbar().y + 4, width - 4, TASKBAR_HEIGHT - 8};
}

static struct rect launcher(void) {
    return (struct rect){4, taskbar().y + 4, LAUNCHER_WIDTH, TASKBAR_HEIGHT - 8};
}

// Text cut to fit a width, which is how titles go on the taskbar.
static void fitted_text(struct gui_canvas *c, int x, int y, int width, const char *text, uint32_t color) {
    char shown[GUI_TITLE_MAX];
    int fits = width / GUI_CHAR_WIDTH;

    snprintf(shown, sizeof(shown), "%.*s", fits > 0 ? fits : 0, text);
    gui_text(c, x, y, shown, color, 1);
}

static void draw_taskbar(struct gui_canvas *c, int ox, int oy) {
    struct rect bar = taskbar(), l = launcher();
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);
    int text_y = bar.y + (TASKBAR_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;
    char clock[8];
    time_t now = time(NULL);

    gui_fill(c, bar.x - ox, bar.y - oy, bar.width, bar.height, TASKBAR_COLOR);
    gui_fill(c, bar.x - ox, bar.y - oy, bar.width, 1, TASKBAR_LINE);
    gui_fill(c, l.x - ox, l.y - oy, l.width, l.height, TASKBAR_BUTTON);
    gui_text(c, l.x + 8 - ox, text_y, "+ Terminal", TITLE_TEXT, 1);

    for (int i = 0; i < count; i++) {
        struct rect b = taskbar_button(i, count);
        struct window *w = list[i];
        uint32_t color = w == focused ? TITLE_FOCUSED : w->minimized ? TASKBAR_MINIMIZED : TASKBAR_BUTTON;

        gui_fill(c, b.x - ox, b.y - oy, b.width, b.height, color);
        fitted_text(c, b.x + 8 - ox, text_y, b.width - 16, w->title, w->minimized ? TASKBAR_DIM_TEXT : TITLE_TEXT);
    }

    strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
    gui_text(c, bar.width - gui_text_width(clock, 1) - 12 - ox, text_y, clock, TITLE_TEXT, 1);
}

static void draw_cursor(struct gui_canvas *c, int ox, int oy) {
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            char p = cursor[row][col];

            if (p != ' ')
                gui_fill(c, cursor_x + col - ox, cursor_y + row - oy, 1, 1, p == 'X' ? 0x000000 : 0xFFFFFF);
        }
    }
}

// Draws everything that overlaps a rectangle, through a canvas that is only that
// part of the screen, then puts it on the framebuffer.
static void compose(struct rect r) {
    struct gui_canvas c = {screen.pixels + r.y * screen.stride + r.x, r.width, r.height, screen.stride};
    int hint_y = desktop().height - 2 * GUI_CHAR_HEIGHT;
    struct rect bar = taskbar();

    gui_fill(&c, 0, 0, r.width, r.height, DESKTOP_COLOR);
    gui_text(&c, GUI_CHAR_WIDTH - r.x, hint_y - r.y, HINT, HINT_COLOR, 1);
    for (int i = 0; i < window_count; i++) {
        struct rect f = frame_of(windows[i]);

        if (!windows[i]->minimized && f.x < r.x + r.width && r.x < f.x + f.width && f.y < r.y + r.height
            && r.y < f.y + f.height)
            draw_window(&c, windows[i], r.x, r.y);
    }
    if (r.y + r.height > bar.y)
        draw_taskbar(&c, r.x, r.y);
    draw_cursor(&c, r.x, r.y);

    for (int row = 0; row < r.height; row++)
        memcpy(framebuffer + (long)(r.y + row) * pitch + r.x * sizeof(uint32_t),
               c.pixels + (long)row * c.stride, r.width * sizeof(uint32_t));
}

static void redraw(void) {
    for (int i = 0; i < damage_count; i++)
        compose(damage[i]);
    damage_count = 0;
}

// Sends without blocking, so a program that stops reading can't stop wm. Mouse
// movement is dropped when it's falling behind, and anything else disconnects it.
static void queue_message(struct client *client, const struct gui_message *m) {
    bool droppable = m->type == GUI_MOUSE_MOVE;

    if (OUTBOX_SIZE - client->out_used < sizeof(*m) * (droppable ? 8 : 1)) {
        client->stuck |= !droppable;
        return;
    }
    memcpy(client->outbox + client->out_used, m, sizeof(*m));
    client->out_used += sizeof(*m);
}

static bool flush_client(struct client *client) {
    if (client->stuck)
        return false;
    while (client->out_used > 0) {
        ssize_t n = write(client->fd, client->outbox, client->out_used);

        if (n < 0)
            return errno == EAGAIN || errno == EINTR;
        memmove(client->outbox, client->outbox + n, client->out_used - n);
        client->out_used -= n;
    }
    return true;
}

static void send_to(struct window *w, struct gui_message m) {
    m.window = w->id;
    queue_message(w->client, &m);
}

static void set_focus(struct window *w) {
    if (w == focused)
        return;
    damage_taskbar();
    if (focused) {
        send_to(focused, (struct gui_message){.type = GUI_FOCUS, .focused = 0});
        damage_title(focused);
    }
    focused = w;
    if (w) {
        send_to(w, (struct gui_message){.type = GUI_FOCUS, .focused = 1});
        damage_title(w);
    }
}

static int index_of(const struct window *w) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i] == w)
            return i;
    }
    return -1;
}

static void raise_window(struct window *w) {
    int i = index_of(w);

    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    windows[window_count - 1] = w;
    add_damage(frame_of(w));
}

// Maps the pixels a message names, and removes the name.
static bool map_buffer(const struct gui_message *m, struct gui_canvas *out) {
    size_t size = (size_t)m->width * m->height * sizeof(uint32_t);
    char buffer[GUI_TITLE_MAX];

    // A name that isn't terminated is cut short rather than trusted.
    memcpy(buffer, m->buffer, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';
    if (m->width <= 0 || m->height <= 0 || m->width > MAX_SIZE || m->height > MAX_SIZE)
        return false;

    int fd = shm_open(buffer, O_RDONLY, 0);

    if (fd < 0)
        return false;
    *out = (struct gui_canvas){mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0), m->width, m->height, m->width};
    shm_unlink(buffer);
    close(fd);
    return out->pixels != MAP_FAILED;
}

static void unmap_buffer(struct gui_canvas *c) {
    munmap(c->pixels, (size_t)c->width * c->height * sizeof(uint32_t));
}

// The top window that isn't minimized gets the focus.
static void focus_top(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (!windows[i]->minimized) {
            set_focus(windows[i]);
            return;
        }
    }
    set_focus(NULL);
}

static void minimize(struct window *w) {
    w->minimized = true;
    add_damage(frame_of(w));
    damage_taskbar();
    if (focused == w)
        focus_top();
}

static void show(struct window *w) {
    w->minimized = false;
    raise_window(w);
    set_focus(w);
    damage_taskbar();
}

static void remove_window(struct window *w) {
    int i = index_of(w);

    add_damage(frame_of(w));
    damage_taskbar();
    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    window_count--;
    if (dragging == w)
        dragging = NULL;
    if (grabbed == w)
        grabbed = NULL;
    if (resizing == w)
        resizing = NULL;
    if (focused == w) {
        focused = NULL;
        focus_top();
    }
    unmap_buffer(&w->canvas);
    free(w);
}

static struct window *find_window(const struct client *client, uint32_t id) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i]->client == client && windows[i]->id == id)
            return windows[i];
    }
    return NULL;
}

static void create_window(struct client *client, const struct gui_message *m) {
    struct gui_message reply = {.type = GUI_CREATED};
    struct window *w = window_count < MAX_WINDOWS ? calloc(1, sizeof(*w)) : NULL;

    if (!w || !map_buffer(m, &w->canvas)) {
        free(w);
        queue_message(client, &reply); // window 0 is a refusal
        return;
    }

    // Each new window goes a little further down and right than the last.
    int step = (next_id - 1) % 8;

    w->client = client;
    w->id = reply.window = next_id++;
    w->x = 80 + step * CASCADE;
    w->y = 60 + TITLE_HEIGHT + step * CASCADE;
    w->want_width = w->canvas.width;
    w->want_height = w->canvas.height;
    memcpy(w->title, m->text, sizeof(w->title));
    w->title[sizeof(w->title) - 1] = '\0';
    windows[window_count++] = w;
    damage_taskbar();
    queue_message(client, &reply);
    add_damage(frame_of(w));
    set_focus(w);
}

// Asks for a new size, unless an answer is still awaited, in which case this is
// asked for once it comes.
static void ask_size(struct window *w, int width, int height) {
    struct rect d = desktop();

    w->want_width = width < MIN_WIDTH ? MIN_WIDTH : width > d.width ? d.width : width;
    w->want_height = height < MIN_HEIGHT ? MIN_HEIGHT : height > d.height ? d.height : height;
    if (w->asking || (w->want_width == w->canvas.width && w->want_height == w->canvas.height))
        return;
    w->asking = true;
    w->asked_width = w->want_width;
    w->asked_height = w->want_height;
    send_to(w, (struct gui_message){.type = GUI_RESIZE, .width = w->want_width, .height = w->want_height});
}

static void take_buffer(struct window *w, const struct gui_message *m) {
    struct gui_canvas c;

    if (!map_buffer(m, &c))
        return;
    add_damage(frame_of(w));
    unmap_buffer(&w->canvas);
    w->canvas = c;
    add_damage(frame_of(w));
    w->asking = false;
    if (w->want_width != w->asked_width || w->want_height != w->asked_height)
        ask_size(w, w->want_width, w->want_height);
}

static void handle_message(struct client *client, const struct gui_message *m) {
    struct window *w = find_window(client, m->window);

    switch (m->type) {
    case GUI_CREATE:
        create_window(client, m);
        break;
    case GUI_DAMAGE:
        if (w)
            add_damage((struct rect){w->x + m->x, w->y + m->y, m->width, m->height});
        break;
    case GUI_DESTROY:
        if (w)
            remove_window(w);
        break;
    case GUI_BUFFER:
        if (w)
            take_buffer(w, m);
        break;
    }
}

static void disconnect(int i) {
    for (int j = window_count - 1; j >= 0; j--) {
        if (windows[j]->client == clients[i])
            remove_window(windows[j]);
    }
    close(clients[i]->fd);
    free(clients[i]);
    clients[i] = NULL;
}

static void read_client(int i) {
    struct client *client = clients[i];
    ssize_t n = read(client->fd, client->inbox + client->in_used, INBOX_SIZE - client->in_used);

    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (n <= 0) {
        disconnect(i);
        return;
    }
    client->in_used += n;

    size_t used = 0;

    for (; client->in_used - used >= sizeof(struct gui_message); used += sizeof(struct gui_message)) {
        struct gui_message m;

        memcpy(&m, client->inbox + used, sizeof(m));
        handle_message(client, &m);
    }
    memmove(client->inbox, client->inbox + used, client->in_used - used);
    client->in_used -= used;
}

static void accept_client(int listener) {
    int fd = accept(listener, NULL, NULL);
    int i = 0;

    if (fd < 0)
        return;
    while (i < MAX_CLIENTS && clients[i])
        i++;
    if (i == MAX_CLIENTS || !(clients[i] = calloc(1, sizeof(struct client)))) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    clients[i]->fd = fd;
}

// Which window is at a point, and which part of it. edges says which edges a
// resize there would move.
static struct window *window_at(int x, int y, enum part *part, int *edges) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = windows[i];
        struct rect f = frame_of(w);

        if (w->minimized || !contains((struct rect){f.x, f.y, f.width + RESIZE_MARGIN, f.height + RESIZE_MARGIN}, x, y))
            continue;
        *edges = (x >= f.x + f.width - BORDER ? EDGE_RIGHT : 0) | (y >= f.y + f.height - BORDER ? EDGE_BOTTOM : 0);
        *part = *edges ? PART_RESIZE
              : contains(button_of(w, 0), x, y) ? PART_CLOSE
              : contains(button_of(w, 1), x, y) ? PART_MAXIMIZE
              : contains(button_of(w, 2), x, y) ? PART_MINIMIZE
              : contains((struct rect){w->x, w->y, w->canvas.width, w->canvas.height}, x, y) ? PART_CONTENT
                                                                                            : PART_TITLE;
        return w;
    }
    *part = PART_NONE;
    return NULL;
}

static void send_mouse(struct window *w, uint32_t type, uint32_t changed, int wheel) {
    send_to(w, (struct gui_message){
                   .type = type, .x = cursor_x - w->x, .y = cursor_y - w->y,
                   .buttons = buttons, .changed = changed, .wheel = wheel,
               });
}

static void launch(const char *command) {
    if (fork() == 0) {
        execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
}

static void move_window(struct window *w, int x, int y) {
    add_damage(frame_of(w));
    w->x = x;
    w->y = y;
    add_damage(frame_of(w));
}

// Puts a window's frame on part of the desktop, keeping where it was to go back to.
static void place(struct window *w, struct rect frame) {
    if (!w->tiled)
        w->restore = frame_of(w);
    w->tiled = true;
    move_window(w, frame.x + BORDER, frame.y + TITLE_HEIGHT + BORDER);
    ask_size(w, frame.width - 2 * BORDER, frame.height - TITLE_HEIGHT - 2 * BORDER);
}

static void untile(struct window *w) {
    struct rect r = w->restore;

    w->tiled = false;
    move_window(w, r.x + BORDER, r.y + TITLE_HEIGHT + BORDER);
    ask_size(w, r.width - 2 * BORDER, r.height - TITLE_HEIGHT - 2 * BORDER);
}

static void toggle_maximized(struct window *w) {
    if (w->tiled)
        untile(w);
    else
        place(w, desktop());
}

static void tile_half(struct window *w, bool right) {
    struct rect d = desktop();

    place(w, (struct rect){right ? d.width / 2 : 0, 0, d.width / 2, d.height});
}

static void taskbar_click(void) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    if (contains(launcher(), cursor_x, cursor_y))
        launch(LAUNCH_COMMAND);
    for (int i = 0; i < count; i++) {
        if (!contains(taskbar_button(i, count), cursor_x, cursor_y))
            continue;
        if (list[i] == focused)
            minimize(list[i]);
        else
            show(list[i]);
    }
}

static void press(uint32_t changed) {
    enum part part;
    int edges;
    struct window *w = window_at(cursor_x, cursor_y, &part, &edges);

    if (contains(taskbar(), cursor_x, cursor_y)) {
        if (changed & MOUSE_BUTTON_LEFT)
            taskbar_click();
        return;
    }
    if (!w)
        return;
    set_focus(w);
    if (w != windows[window_count - 1])
        raise_window(w);

    if (part == PART_CONTENT) {
        grabbed = w;
        send_mouse(w, GUI_MOUSE_BUTTON, changed, 0);
    } else if (part == PART_CLOSE) {
        send_to(w, (struct gui_message){.type = GUI_CLOSE});
    } else if (part == PART_MAXIMIZE) {
        toggle_maximized(w);
    } else if (part == PART_MINIMIZE) {
        minimize(w);
    } else if (part == PART_RESIZE && (changed & MOUSE_BUTTON_LEFT)) {
        resizing = w;
        resize_edges = edges;
        resize_x = cursor_x;
        resize_y = cursor_y;
        resize_width = w->canvas.width;
        resize_height = w->canvas.height;
        w->tiled = false;
    } else if (changed & MOUSE_BUTTON_LEFT) {
        w->tiled = false;
        dragging = w;
        drag_x = cursor_x - w->x;
        drag_y = cursor_y - w->y;
    }
}

static void handle_mouse(const struct mouse_event *e) {
    enum part part;
    int edges;
    uint32_t pressed = e->buttons & ~buttons, released = buttons & ~e->buttons;

    if (e->dx || e->dy) {
        damage_cursor();
        cursor_x = cursor_x + e->dx < 0 ? 0 : cursor_x + e->dx >= screen.width ? screen.width - 1 : cursor_x + e->dx;
        cursor_y = cursor_y + e->dy < 0 ? 0 : cursor_y + e->dy >= screen.height ? screen.height - 1 : cursor_y + e->dy;
        damage_cursor();
    }
    buttons = e->buttons;

    // Windows can be dragged partly off the screen, but the title bar stays on it.
    if (dragging && (e->dx || e->dy)) {
        int y = cursor_y - drag_y;

        int bottom = desktop().height - BORDER;

        move_window(dragging, cursor_x - drag_x, y < TITLE_HEIGHT ? TITLE_HEIGHT : y > bottom ? bottom : y);
    }
    if (resizing && (e->dx || e->dy))
        ask_size(resizing, resize_width + (resize_edges & EDGE_RIGHT ? cursor_x - resize_x : 0),
                 resize_height + (resize_edges & EDGE_BOTTOM ? cursor_y - resize_y : 0));

    struct window *target = grabbed ? grabbed : dragging || resizing ? NULL : window_at(cursor_x, cursor_y, &part, &edges);

    if (target && !grabbed && part != PART_CONTENT)
        target = NULL;
    if (target && (e->dx || e->dy))
        send_mouse(target, GUI_MOUSE_MOVE, 0, 0);
    if (target && e->wheel)
        send_mouse(target, GUI_MOUSE_WHEEL, 0, e->wheel);

    if (pressed)
        press(pressed);
    if (released && grabbed)
        send_mouse(grabbed, GUI_MOUSE_BUTTON, released, 0);
    if (!buttons) {
        dragging = NULL;
        grabbed = NULL;
        resizing = NULL;
    }
}

// Super with something else is for wm, and every other key goes to the focused window.
static void handle_key(const struct key_event *e) {
    bool super = e->modifiers & KEY_MOD_SUPER;

    if (super && e->code == 0x10) {
        if (e->pressed && (e->modifiers & KEY_MOD_SHIFT))
            quitting = true;
        else if (e->pressed && focused)
            send_to(focused, (struct gui_message){.type = GUI_CLOSE});
        return;
    }
    if (super && e->code == KEY_ENTER) {
        if (e->pressed)
            launch(LAUNCH_COMMAND);
        return;
    }

    // Up maximizes, left and right take half the screen, and down goes back or minimizes.
    if (super && (e->code == KEY_UP || e->code == KEY_DOWN || e->code == KEY_LEFT || e->code == KEY_RIGHT)) {
        if (!e->pressed || !focused)
            return;
        if (e->code == KEY_UP)
            toggle_maximized(focused);
        else if (e->code == KEY_DOWN && focused->tiled)
            untile(focused);
        else if (e->code == KEY_DOWN)
            minimize(focused);
        else
            tile_half(focused, e->code == KEY_RIGHT);
        return;
    }
    if (focused)
        send_to(focused, (struct gui_message){.type = GUI_KEY, .key = *e});
}

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "wm: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: wm [command]...\n"
                            "Runs the window system, starting each command in it. Super+Shift+Q quits.\n");
    signal(SIGPIPE, SIG_IGN);

    struct fb_info info;
    int fb = open("/dev/fb0", O_RDWR);

    if (fb < 0 || fb_info(fb, &info) < 0)
        return fail("/dev/fb0");

    int mouse = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
    int keyboard = open("/dev/keyboard", O_RDONLY | O_NONBLOCK);
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = GUI_SOCKET};
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);

    if (mouse < 0)
        return fail("/dev/mouse");
    if (keyboard < 0)
        return fail("/dev/keyboard");
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 8) < 0)
        return fail(GUI_SOCKET);
    fcntl(listener, F_SETFL, O_NONBLOCK);

    pitch = info.pitch;
    framebuffer = mmap(NULL, (size_t)pitch * info.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    screen = (struct gui_canvas){malloc((size_t)info.width * info.height * sizeof(uint32_t)), info.width,
                                 info.height, info.width};
    if (framebuffer == MAP_FAILED || !screen.pixels)
        return fail("screen");

    cursor_x = screen.width / 2;
    cursor_y = screen.height / 2;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    for (int i = 1; i < argc; i++)
        launch(argv[i]);
    printf("wm: running at %dx%d\n", screen.width, screen.height);
    fflush(stdout);

    while (!quitting) {
        struct pollfd fds[3 + MAX_CLIENTS] = {
            {.fd = listener, .events = POLLIN}, {.fd = mouse, .events = POLLIN}, {.fd = keyboard, .events = POLLIN},
        };

        for (int i = 0; i < MAX_CLIENTS; i++) {
            fds[3 + i].fd = clients[i] ? clients[i]->fd : -1;
            fds[3 + i].events = clients[i] ? POLLIN | (clients[i]->out_used ? POLLOUT : 0) : 0;
        }
        redraw();

        // The clock changes at the start of each minute.
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        int until_minute = (60 - now.tv_sec % 60) * 1000 - now.tv_nsec / 1000000;

        int ready = poll(fds, 3 + MAX_CLIENTS, until_minute > 0 ? until_minute : 1);

        if (ready < 0 && errno != EINTR)
            return fail("poll");
        if (ready == 0)
            damage_taskbar();

        if (fds[0].revents & POLLIN)
            accept_client(listener);

        struct mouse_event mouse_events[32];
        ssize_t n = fds[1].revents & POLLIN ? read(mouse, mouse_events, sizeof(mouse_events)) : 0;

        for (ssize_t i = 0; i < n / (ssize_t)sizeof(*mouse_events); i++)
            handle_mouse(&mouse_events[i]);

        struct key_event key_events[32];
        n = fds[2].revents & POLLIN ? read(keyboard, key_events, sizeof(key_events)) : 0;
        for (ssize_t i = 0; i < n / (ssize_t)sizeof(*key_events); i++)
            handle_key(&key_events[i]);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && (fds[3 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                read_client(i);
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && !flush_client(clients[i]))
                disconnect(i);
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
    printf("wm: quit\n");
    return 0;
}
