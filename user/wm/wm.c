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
#include <unistd.h>

#define MAX_CLIENTS 32
#define MAX_WINDOWS 64
#define MAX_DAMAGE 32
#define MAX_SIZE 4096
#define INBOX_SIZE (8 * sizeof(struct gui_message))
#define OUTBOX_SIZE (64 * sizeof(struct gui_message))

#define TITLE_HEIGHT 24
#define BORDER 1
#define CLOSE_SIZE 16
#define CASCADE 32

#define DESKTOP_COLOR 0x1E2430
#define HINT_COLOR 0x5C677D
#define BORDER_COLOR 0x0E1118
#define TITLE_FOCUSED 0x3D63DD
#define TITLE_UNFOCUSED 0x3A4150
#define TITLE_TEXT 0xFFFFFF
#define CLOSE_COLOR 0xE5484D

// Super+Enter runs this.
#define LAUNCH_COMMAND "term"
#define HINT "Super+Enter: terminal   Super+Q: close   Super+Shift+Q: quit"

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
};

enum part { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE };

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

static struct rect close_of(const struct window *w) {
    return (struct rect){w->x + w->canvas.width - CLOSE_SIZE - (TITLE_HEIGHT - CLOSE_SIZE) / 2,
                         w->y - TITLE_HEIGHT + (TITLE_HEIGHT - CLOSE_SIZE) / 2, CLOSE_SIZE, CLOSE_SIZE};
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

static void damage_cursor(void) {
    add_damage((struct rect){cursor_x, cursor_y, CURSOR_WIDTH, CURSOR_HEIGHT});
}

static void draw_window(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    struct rect frame = frame_of(w), close = close_of(w);
    uint32_t title_color = w == focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
    int text_y = w->y - TITLE_HEIGHT + (TITLE_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;

    gui_fill(c, frame.x - ox, frame.y - oy, frame.width, frame.height, BORDER_COLOR);
    gui_fill(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT, title_color);
    gui_text(c, w->x + 8 - ox, text_y, w->title, TITLE_TEXT, 1);
    gui_fill(c, close.x - ox, close.y - oy, close.width, close.height, CLOSE_COLOR);
    gui_text(c, close.x + (CLOSE_SIZE - GUI_CHAR_WIDTH) / 2 - ox, text_y, "x", TITLE_TEXT, 1);
    gui_copy(c, w->x - ox, w->y - oy, &w->canvas, 0, 0, w->canvas.width, w->canvas.height);
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
    int hint_y = screen.height - 2 * GUI_CHAR_HEIGHT;

    gui_fill(&c, 0, 0, r.width, r.height, DESKTOP_COLOR);
    gui_text(&c, GUI_CHAR_WIDTH - r.x, hint_y - r.y, HINT, HINT_COLOR, 1);
    for (int i = 0; i < window_count; i++) {
        struct rect f = frame_of(windows[i]);

        if (f.x < r.x + r.width && r.x < f.x + f.width && f.y < r.y + r.height && r.y < f.y + f.height)
            draw_window(&c, windows[i], r.x, r.y);
    }
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

static void remove_window(struct window *w) {
    int i = index_of(w);

    add_damage(frame_of(w));
    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    window_count--;
    if (dragging == w)
        dragging = NULL;
    if (grabbed == w)
        grabbed = NULL;
    if (focused == w) {
        focused = NULL;
        set_focus(window_count ? windows[window_count - 1] : NULL);
    }
    munmap(w->canvas.pixels, (size_t)w->canvas.width * w->canvas.height * sizeof(uint32_t));
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
    struct window *w = NULL;
    size_t size = (size_t)m->width * m->height * sizeof(uint32_t);
    int fd = -1;

    // A name and title that aren't terminated are cut short rather than trusted.
    char buffer[GUI_TITLE_MAX];
    memcpy(buffer, m->buffer, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';

    if (window_count < MAX_WINDOWS && m->width > 0 && m->height > 0 && m->width <= MAX_SIZE && m->height <= MAX_SIZE
        && (w = calloc(1, sizeof(*w))) && (fd = shm_open(buffer, O_RDONLY, 0)) >= 0) {
        w->canvas = (struct gui_canvas){mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0), m->width, m->height, m->width};
        shm_unlink(buffer);
    }
    if (fd >= 0)
        close(fd);
    if (!w || fd < 0 || w->canvas.pixels == MAP_FAILED) {
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
    memcpy(w->title, m->text, sizeof(w->title));
    w->title[sizeof(w->title) - 1] = '\0';
    windows[window_count++] = w;
    queue_message(client, &reply);
    add_damage(frame_of(w));
    set_focus(w);
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

static struct window *window_at(int x, int y, enum part *part) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = windows[i];

        if (!contains(frame_of(w), x, y))
            continue;
        *part = contains(close_of(w), x, y) ? PART_CLOSE
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

static void move_window(struct window *w, int x, int y) {
    add_damage(frame_of(w));
    w->x = x;
    w->y = y;
    add_damage(frame_of(w));
}

static void press(uint32_t changed) {
    enum part part;
    struct window *w = window_at(cursor_x, cursor_y, &part);

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
    } else if (changed & MOUSE_BUTTON_LEFT) {
        dragging = w;
        drag_x = cursor_x - w->x;
        drag_y = cursor_y - w->y;
    }
}

static void handle_mouse(const struct mouse_event *e) {
    enum part part;
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

        move_window(dragging, cursor_x - drag_x, y < TITLE_HEIGHT ? TITLE_HEIGHT : y);
    }

    struct window *target = grabbed ? grabbed : dragging ? NULL : window_at(cursor_x, cursor_y, &part);

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
    }
}

static void launch(const char *command) {
    if (fork() == 0) {
        execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
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
        if (poll(fds, 3 + MAX_CLIENTS, -1) < 0 && errno != EINTR)
            return fail("poll");

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
