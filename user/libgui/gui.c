#include "backend.h"
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connection = -1;

static bool transfer(int fd, void *data, bool sending) {
    for (size_t done = 0; done < sizeof(struct gui_message);) {
        ssize_t n = sending ? write(fd, (char *)data + done, sizeof(struct gui_message) - done)
                            : read(fd, (char *)data + done, sizeof(struct gui_message) - done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

bool gui_receive(int fd, struct gui_message *message) {
    return transfer(fd, message, false);
}

bool gui_send(int fd, const struct gui_message *message) {
    return transfer(fd, (void *)message, true);
}

bool gui_connect(void) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    const char *given = getenv("GUI_FD"), *path = getenv("GUI_SOCKET");

    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path ? path : GUI_SOCKET);
    if (given) {
        connection = atoi(given);
        unsetenv("GUI_FD");
        fcntl(connection, F_SETFD, FD_CLOEXEC);
        return true;
    }

    connection = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); // not for programs started from here
    if (connection >= 0 && connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0)
        return true;
    close(connection);
    connection = -1;
    return false;
}

int gui_fd(void) {
    return connection;
}

// Shared memory, which wm maps and then removes the name of.
static void *new_shared(size_t size, char name[GUI_TITLE_MAX]) {
    static int created;

    snprintf(name, GUI_TITLE_MAX, "/gui-%d-%d", getpid(), created++);

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    void *shared = fd < 0 || ftruncate(fd, size) < 0 ? MAP_FAILED
                                                     : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (fd >= 0)
        close(fd);
    if (shared == MAP_FAILED) {
        shm_unlink(name);
        return NULL;
    }
    return shared;
}

// For pixels, width by height.
static uint32_t *new_buffer(int width, int height, char name[GUI_TITLE_MAX]) {
    return width > 0 && height > 0 ? new_shared((size_t)width * height * sizeof(uint32_t), name) : NULL;
}

static size_t canvas_size(const struct gui_canvas *c) {
    return (size_t)c->width * c->height * sizeof(uint32_t);
}

// A blank canvas, the size of the shown pixels, replacing the old one.
static bool new_canvas(struct gui_window *window, int width, int height) {
    uint32_t *pixels = calloc((size_t)width * height, sizeof(uint32_t));

    if (!pixels)
        return false;
    free(window->canvas.pixels);
    window->canvas = (struct gui_canvas){.pixels = pixels, .width = width, .height = height, .stride = width};
    return true;
}

struct gui_window *gui_create_window(const char *title, int width, int height) {
    struct gui_message message = {.type = GUI_CREATE, .width = width, .height = height};
    struct gui_window *window = calloc(1, sizeof(*window));
    uint32_t *shown = window ? new_buffer(width, height, message.buffer) : NULL;

    snprintf(message.text, sizeof(message.text), "%s", title);
    if (!shown || !new_canvas(window, width, height) || !gui_send(connection, &message)) {
        if (shown) {
            shm_unlink(message.buffer);
            munmap(shown, (size_t)width * height * sizeof(uint32_t));
        }
        if (window)
            free(window->canvas.pixels);
        free(window);
        return NULL;
    }

    window->shown = shown;
    while (gui_receive(connection, &message)) {
        if (message.type == GUI_CREATED && message.window) {
            const char *name = program_invocation_name ? strrchr(program_invocation_name, '/') : NULL;

            window->id = message.window;
            gui_set_icon(window, name ? name + 1 : program_invocation_name ? program_invocation_name : "");
            return window;
        }
        if (message.type == GUI_CREATED)
            break;
        gui_post(&message);
    }
    munmap(shown, canvas_size(&window->canvas));
    free(window->canvas.pixels);
    free(window);
    return NULL;
}

bool gui_resize(struct gui_window *window, int width, int height) {
    char name[GUI_TITLE_MAX];
    uint32_t *shown = new_buffer(width, height, name);
    size_t size = canvas_size(&window->canvas);

    if (shown && !new_canvas(window, width, height)) {
        shm_unlink(name);
        munmap(shown, (size_t)width * height * sizeof(uint32_t));
        shown = NULL;
    }
    if (!shown)
        return false;

    // Pixels wm never saw can go at once.
    if (window->pending[0]) {
        shm_unlink(window->pending);
        munmap(window->shown, size);
    } else {
        window->old_shown = window->shown;
        window->old_size = size;
    }
    memcpy(window->pending, name, sizeof(name));
    window->shown = shown;
    return true;
}

void gui_destroy_window(struct gui_window *window) {
    struct gui_message message = {.type = GUI_DESTROY, .window = window->id};

    gui_send(connection, &message);
    if (window->pending[0]) {
        shm_unlink(window->pending);
        munmap(window->old_shown, window->old_size);
    }
    munmap(window->shown, canvas_size(&window->canvas));
    free(window->canvas.pixels);
    free(window);
}

static void copy_shown(struct gui_window *window, int x, int y, int width, int height) {
    struct gui_canvas shown = {window->shown, window->canvas.width, window->canvas.height, window->canvas.width};

    gui_copy(&shown, x, y, &window->canvas, x, y, width, height);
}

// Copies what changed in part of the canvas to the shown pixels, row by row, and
// gives the part that holds all the changes, which has no width if nothing did.
static void copy_changes(struct gui_window *window, struct gui_message *changed) {
    const struct gui_canvas *c = &window->canvas;
    int x = changed->x < 0 ? 0 : changed->x, y = changed->y < 0 ? 0 : changed->y;
    int right = changed->x + changed->width < c->width ? changed->x + changed->width : c->width;
    int bottom = changed->y + changed->height < c->height ? changed->y + changed->height : c->height;
    int top = -1, last = 0, left = right, end = x;

    for (int row = y; row < bottom; row++) {
        const uint32_t *from = c->pixels + (long)row * c->stride;
        uint32_t *to = window->shown + (long)row * c->width;
        int l = x, r = right - 1;

        while (l < right && from[l] == to[l])
            l++;
        if (l == right)
            continue;
        while (from[r] == to[r])
            r--;
        memcpy(to + l, from + l, (r - l + 1) * sizeof(uint32_t));
        top = top < 0 ? row : top;
        last = row;
        left = l < left ? l : left;
        end = r + 1 > end ? r + 1 : end;
    }
    *changed = (struct gui_message){
        .type = GUI_DAMAGE, .window = window->id, .x = left, .y = top, .width = top < 0 ? 0 : end - left,
        .height = last - top + 1,
    };
}

// Only what changed goes to wm, so a frame drawn again the same costs it nothing.
void gui_update(struct gui_window *window, int x, int y, int width, int height) {
    struct gui_message message = {
        .type = GUI_DAMAGE, .window = window->id, .x = x, .y = y, .width = width, .height = height,
    };

    if (window->pending[0]) {
        struct gui_message buffer = {
            .type = GUI_BUFFER, .window = window->id, .width = window->canvas.width, .height = window->canvas.height,
        };

        copy_shown(window, 0, 0, window->canvas.width, window->canvas.height);
        memcpy(buffer.buffer, window->pending, sizeof(buffer.buffer));
        gui_send(connection, &buffer);
        munmap(window->old_shown, window->old_size);
        window->pending[0] = '\0';
    } else {
        copy_changes(window, &message);
        if (message.width <= 0)
            return;
    }
    gui_send(connection, &message);
}

void gui_set_title(struct gui_window *window, const char *title) {
    struct gui_message message = {.type = GUI_TITLE, .window = window->id};

    snprintf(message.text, sizeof(message.text), "%s", title);
    gui_send(connection, &message);
}

void gui_set_icon(struct gui_window *window, const char *name) {
    struct gui_message message = {.type = GUI_ICON, .window = window->id};

    snprintf(message.text, sizeof(message.text), "%s", name);
    gui_send(connection, &message);
}

void gui_set_cursor(struct gui_window *window, enum gui_cursor cursor) {
    gui_send(connection, &(struct gui_message){.type = GUI_CURSOR, .window = window->id, .x = cursor});
}

void gui_set_translucent(struct gui_window *window, bool on) {
    gui_send(connection, &(struct gui_message){.type = GUI_TRANSLUCENT, .window = window->id, .x = on});
}

// Sends a message with data in new shared memory, which wm maps and removes.
static bool send_shared(struct gui_message message, const void *data, size_t length) {
    message.width = (int32_t)length;

    char *shared = length ? new_shared(length, message.buffer) : NULL;

    if (length && !shared)
        return false;
    if (shared) {
        memcpy(shared, data, length);
        munmap(shared, length);
    }
    return gui_send(connection, &message);
}

// A copy, ending in a zero byte, of the shared memory a message from wm names, which
// is then removed, or NULL if there's none or it's over max bytes.
static char *take_shared(const struct gui_message *message, size_t max) {
    char name[GUI_TITLE_MAX];
    size_t size = message->width > 0 && (size_t)message->width <= max ? (size_t)message->width : 0;

    snprintf(name, sizeof(name), "%.*s", GUI_TITLE_MAX - 1, message->buffer);
    if (!size)
        return NULL;

    int fd = shm_open(name, O_RDONLY, 0);
    void *shared = fd < 0 ? MAP_FAILED : mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    char *copy = shared == MAP_FAILED ? NULL : malloc(size + 1);

    if (fd >= 0)
        close(fd);
    shm_unlink(name);
    if (copy) {
        memcpy(copy, shared, size);
        copy[size] = '\0';
    }
    if (shared != MAP_FAILED)
        munmap(shared, size);
    return copy;
}

bool gui_clipboard_set(const char *text, size_t length) {
    return send_shared((struct gui_message){.type = GUI_CLIPBOARD_SET}, text, length);
}

bool gui_clipboard_set_image(const struct gui_canvas *from) {
    size_t row = (size_t)from->width * sizeof(uint32_t);
    uint32_t *pixels = malloc(row * from->height);
    bool ok;

    if (!pixels)
        return false;
    for (int y = 0; y < from->height; y++)
        memcpy(pixels + (size_t)y * from->width, from->pixels + (size_t)y * from->stride, row);
    ok = send_shared((struct gui_message){.type = GUI_CLIPBOARD_SET, .text = "image", .x = from->width, .y = from->height},
                     pixels, row * from->height);
    free(pixels);
    return ok;
}

bool gui_drag_file(const char *path) {
    return send_shared((struct gui_message){.type = GUI_DRAG}, path, strlen(path));
}

bool gui_dropped_file(const struct gui_message *drop, char path[GUI_PATH_MAX]) {
    char *copy = drop->type == GUI_DROP ? take_shared(drop, GUI_PATH_MAX - 1) : NULL;

    if (copy)
        strcpy(path, copy);
    free(copy);
    return copy;
}

bool gui_notify_action(struct gui_window *window, const char *title, const char *text, const char *label,
                       const char *command) {
    struct gui_message message = {.type = GUI_NOTIFY, .window = window ? window->id : 0};

    snprintf(message.text, sizeof(message.text), "%s", title);
    snprintf(message.buffer, sizeof(message.buffer), "%s", text ? text : "");
    if (!gui_send(connection, &message))
        return false;
    if (!label)
        return true;
    message.type = GUI_NOTIFY_ACTION;
    snprintf(message.text, sizeof(message.text), "%s", label);
    snprintf(message.buffer, sizeof(message.buffer), "%s", command);
    return gui_send(connection, &message);
}

bool gui_notify(struct gui_window *window, const char *title, const char *text) {
    return gui_notify_action(window, title, text, NULL, NULL);
}

// What's on the clipboard, and whether it's a picture, waiting for the answer and
// keeping events that come first for gui_next_event.
static char *clipboard(struct gui_message *message, bool *image) {
    *message = (struct gui_message){.type = GUI_CLIPBOARD_GET};
    *image = false;
    if (!gui_send(connection, message))
        return NULL;
    while (gui_receive(connection, message) && message->type != GUI_CLIPBOARD)
        gui_post(message);
    *image = !strncmp(message->text, "image", sizeof(message->text));
    return message->type == GUI_CLIPBOARD ? take_shared(message, INT32_MAX) : NULL;
}

char *gui_clipboard_get(size_t *length) {
    struct gui_message message;
    bool image;
    char *text = clipboard(&message, &image);

    if (image) {
        free(text);
        text = NULL;
    }
    if (length)
        *length = text ? (size_t)message.width : 0;
    return text;
}

bool gui_clipboard_get_image(struct gui_image *out) {
    struct gui_message message;
    bool image;
    uint32_t *pixels = (uint32_t *)clipboard(&message, &image);
    size_t count = (size_t)message.x * message.y;

    if (!pixels || !image || message.x <= 0 || message.y <= 0 || count * sizeof(uint32_t) != (size_t)message.width) {
        free(pixels);
        return false;
    }
    for (size_t i = 0; i < count; i++)
        pixels[i] |= 0xFF000000; // nothing's see-through
    *out = (struct gui_image){pixels, message.x, message.y};
    return true;
}

void gui_show(struct gui_window *window) {
    gui_update(window, 0, 0, window->canvas.width, window->canvas.height);
}

// Waits for wm, until due if it isn't -1, putting what it sends with the events.
bool gui_backend_wait(int64_t due) {
    struct gui_message event;
    struct pollfd fd = {.fd = connection, .events = POLLIN};

    for (;;) {
        int64_t wait = due - gui_now_ms();
        int ready = due < 0 ? 1 : poll(&fd, 1, wait > 0 ? (int)wait : 0);

        if (ready < 0 && errno == EINTR)
            continue;
        if (ready == 0)
            return true;
        if (!gui_receive(connection, &event)) // an error poll found shows up here too
            return false;
        gui_post(&event);
        return true;
    }
}
